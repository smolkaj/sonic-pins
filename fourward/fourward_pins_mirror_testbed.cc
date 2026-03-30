// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fourward/fourward_pins_mirror_testbed.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_pins_switch.h"
#include "grpcpp/client_context.h"
#include "gutil/status.h"

namespace dvaas {

using fourward::dataplane::Dataplane;
using fourward::dataplane::InjectPacketRequest;
using fourward::dataplane::InjectPacketResponse;
using fourward::dataplane::OutputPacket;
using fourward::dataplane::PacketSet;
using fourward::dataplane::ProcessPacketResult;
using fourward::dataplane::SubscribeResultsRequest;
using fourward::dataplane::SubscribeResultsResponse;

absl::StatusOr<std::unique_ptr<FourwardPinsMirrorTestbed>>
FourwardPinsMirrorTestbed::Create(uint32_t sut_device_id,
                                  uint32_t control_device_id) {
  ASSIGN_OR_RETURN(
      FourwardPinsSwitch sut_switch,
      FourwardPinsSwitch::Create({.device_id = sut_device_id}));
  ASSIGN_OR_RETURN(
      FourwardPinsSwitch control_switch,
      FourwardPinsSwitch::Create({.device_id = control_device_id}));

  // Can't use make_unique due to private constructor.
  return std::unique_ptr<FourwardPinsMirrorTestbed>(
      new FourwardPinsMirrorTestbed(std::move(sut_switch),
                                    std::move(control_switch)));
}

FourwardPinsMirrorTestbed::~FourwardPinsMirrorTestbed() { StopBridge(); }

absl::Status FourwardPinsMirrorTestbed::StartBridge() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return absl::FailedPreconditionError("Bridge already running");
  }

  sut_to_control_ =
      std::thread(&FourwardPinsMirrorTestbed::ForwardLoop, this,
                  std::ref(sut_), std::ref(control_), "SUT->Control",
                  std::ref(sut_to_control_ready_));
  control_to_sut_ =
      std::thread(&FourwardPinsMirrorTestbed::ForwardLoop, this,
                  std::ref(control_), std::ref(sut_), "Control->SUT",
                  std::ref(control_to_sut_ready_));

  constexpr absl::Duration kTimeout = absl::Seconds(10);
  if (!sut_to_control_ready_.WaitForNotificationWithTimeout(kTimeout) ||
      !control_to_sut_ready_.WaitForNotificationWithTimeout(kTimeout)) {
    StopBridge();
    return absl::DeadlineExceededError(
        "Bridge subscriptions did not become active within 10s");
  }

  LOG(INFO) << "Packet bridge started: " << sut_.ChassisName() << " <-> "
            << control_.ChassisName();
  return absl::OkStatus();
}

void FourwardPinsMirrorTestbed::StopBridge() {
  if (!running_.load()) return;
  running_.store(false);
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (sut_subscribe_ctx_ != nullptr) sut_subscribe_ctx_->TryCancel();
    if (control_subscribe_ctx_ != nullptr) control_subscribe_ctx_->TryCancel();
  }
  if (sut_to_control_.joinable()) sut_to_control_.join();
  if (control_to_sut_.joinable()) control_to_sut_.join();
  LOG(INFO) << "Packet bridge stopped. Forwarded: " << packets_forwarded_.load()
            << ", inject failures: " << inject_failures_.load();
}

absl::StatusOr<uint32_t> FourwardPinsMirrorTestbed::ResolveTargetPort(
    FourwardPinsSwitch& from_switch, FourwardPinsSwitch& to_switch,
    uint32_t egress_port) {
  // Look up the interface name for the egress port on the source switch.
  std::vector<FakeInterface> from_interfaces =
      from_switch.GnmiService().Interfaces();
  std::string interface_name;
  for (const FakeInterface& iface : from_interfaces) {
    // P4RT ID = dataplane port (identity mapping per design).
    if (static_cast<uint32_t>(iface.p4rt_id) == egress_port) {
      interface_name = iface.name;
      break;
    }
  }
  if (interface_name.empty()) {
    return absl::NotFoundError(
        absl::StrFormat("No interface with P4RT ID %d on source switch",
                        egress_port));
  }

  // Find the corresponding port on the target switch.
  std::vector<FakeInterface> to_interfaces =
      to_switch.GnmiService().Interfaces();
  for (const FakeInterface& iface : to_interfaces) {
    if (iface.name == interface_name) {
      return static_cast<uint32_t>(iface.p4rt_id);
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("No interface named '%s' on target switch",
                      interface_name));
}

void FourwardPinsMirrorTestbed::ForwardLoop(
    FourwardPinsSwitch& from_switch, FourwardPinsSwitch& to_switch,
    const std::string& direction_label, absl::Notification& ready) {
  std::unique_ptr<Dataplane::Stub> from_stub = from_switch.CreateDataplaneStub();
  std::unique_ptr<Dataplane::Stub> to_stub = to_switch.CreateDataplaneStub();

  grpc::ClientContext subscribe_ctx;
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (&from_switch == &sut_)
      sut_subscribe_ctx_ = &subscribe_ctx;
    else
      control_subscribe_ctx_ = &subscribe_ctx;
  }
  SubscribeResultsRequest subscribe_request;
  std::unique_ptr<grpc::ClientReaderInterface<SubscribeResultsResponse>>
      reader = from_stub->SubscribeResults(&subscribe_ctx, subscribe_request);

  LOG(INFO) << direction_label << ": subscribed to " << from_switch.ChassisName();

  SubscribeResultsResponse response;
  while (running_.load() && reader->Read(&response)) {
    switch (response.event_case()) {
      case SubscribeResultsResponse::kActive:
        LOG(INFO) << direction_label << ": subscription active";
        ready.Notify();
        continue;
      case SubscribeResultsResponse::kResult:
        break;
      case SubscribeResultsResponse::EVENT_NOT_SET:
        continue;
    }

    const ProcessPacketResult& result = response.result();

    // Forward each output packet from the first possible outcome to the
    // other instance.  Only the first outcome is forwarded because real
    // hardware executes exactly one alternative; forwarding all would
    // duplicate packets for nondeterministic programs.
    for (const PacketSet& outcome : result.possible_outcomes()) {
      for (const OutputPacket& output : outcome.packets()) {
        absl::StatusOr<uint32_t> target_port =
            ResolveTargetPort(from_switch, to_switch,
                              output.dataplane_egress_port());
        if (!target_port.ok()) {
          LOG(WARNING) << direction_label
                       << ": failed to resolve target port for egress port "
                       << output.dataplane_egress_port() << ": "
                       << target_port.status();
          inject_failures_.fetch_add(1);
          continue;
        }

        InjectPacketRequest inject_request;
        inject_request.set_dataplane_ingress_port(*target_port);
        inject_request.set_payload(output.payload());

        grpc::ClientContext inject_ctx;
        InjectPacketResponse inject_response;
        grpc::Status inject_status =
            to_stub->InjectPacket(&inject_ctx, inject_request,
                                  &inject_response);
        if (inject_status.ok()) {
          packets_forwarded_.fetch_add(1);
        } else {
          LOG(WARNING) << direction_label << ": InjectPacket failed: "
                       << inject_status.error_message();
          inject_failures_.fetch_add(1);
        }
      }
      // Only forward the first possible outcome.
      break;
    }
  }

  // Deregister context before finishing.
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (&from_switch == &sut_)
      sut_subscribe_ctx_ = nullptr;
    else
      control_subscribe_ctx_ = nullptr;
  }
  reader->Finish();
  LOG(INFO) << direction_label << ": stopped";
}

}  // namespace dvaas
