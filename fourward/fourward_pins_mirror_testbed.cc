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
#include <mutex>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "fourward/fourward_pins_switch.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.pb.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/status.h"
#include "re2/re2.h"

namespace dvaas {
namespace {

using fourward::dataplane::Dataplane;
using fourward::dataplane::InjectPacketRequest;
using fourward::dataplane::InjectPacketResponse;
using fourward::dataplane::OutputPacket;
using fourward::dataplane::PacketSet;
using fourward::dataplane::ProcessPacketResult;
using fourward::dataplane::SubscribeResultsRequest;
using fourward::dataplane::SubscribeResultsResponse;

// Matches interface entries in FakeGnmiService state JSON, e.g.:
//   "state":{"name":"Ethernet0",...,"openconfig-p4rt:id":1}
const re2::RE2& InterfaceStatePattern() {
  static const re2::RE2* pattern = new re2::RE2(
      R"re("state":\{"name":"([^"]+)"[^}]*"openconfig-p4rt:id":(\d+)\})re");
  return *pattern;
}

absl::StatusOr<std::string> GetGnmiStateJson(
    gnmi::gNMI::StubInterface& gnmi_stub) {
  gnmi::GetRequest request;
  request.set_type(gnmi::GetRequest::STATE);
  grpc::ClientContext ctx;
  gnmi::GetResponse response;
  grpc::Status status = gnmi_stub.Get(&ctx, request, &response);
  if (!status.ok()) {
    return absl::InternalError(
        absl::StrCat("gNMI Get failed: ", status.error_message()));
  }
  if (response.notification_size() == 0 ||
      response.notification(0).update_size() == 0) {
    return absl::InternalError("gNMI Get returned empty response");
  }
  return response.notification(0).update(0).val().json_ietf_val();
}

// Looks up the gNMI interface name for a dataplane port.
// Uses the identity mapping: P4RT ID = dataplane port number.
absl::StatusOr<std::string> InterfaceNameForPort(
    gnmi::gNMI::StubInterface& gnmi_stub, uint32_t dataplane_port) {
  ASSIGN_OR_RETURN(std::string json, GetGnmiStateJson(gnmi_stub));
  int p4rt_id = static_cast<int>(dataplane_port);
  re2::StringPiece input(json);
  std::string name;
  int id;
  while (RE2::FindAndConsume(&input, InterfaceStatePattern(), &name, &id)) {
    if (id == p4rt_id) return name;
  }
  return absl::NotFoundError(
      absl::StrFormat("No interface with P4RT ID %d", p4rt_id));
}

// Looks up the dataplane port for a gNMI interface name.
// Uses the identity mapping: dataplane port number = P4RT ID.
absl::StatusOr<uint32_t> PortForInterfaceName(
    gnmi::gNMI::StubInterface& gnmi_stub,
    const std::string& interface_name) {
  ASSIGN_OR_RETURN(std::string json, GetGnmiStateJson(gnmi_stub));
  re2::StringPiece input(json);
  std::string name;
  int id;
  while (RE2::FindAndConsume(&input, InterfaceStatePattern(), &name, &id)) {
    if (name == interface_name) return static_cast<uint32_t>(id);
  }
  return absl::NotFoundError(
      absl::StrFormat("No interface named '%s'", interface_name));
}

}  // namespace

absl::StatusOr<std::unique_ptr<FourwardPinsMirrorTestbed>>
FourwardPinsMirrorTestbed::Create(uint32_t sut_device_id,
                                  uint32_t control_device_id) {
  ASSIGN_OR_RETURN(FourwardPinsSwitch sut_switch,
                   FourwardPinsSwitch::Create(sut_device_id));
  ASSIGN_OR_RETURN(FourwardPinsSwitch control_switch,
                   FourwardPinsSwitch::Create(control_device_id));

  // Can't use make_unique due to private constructor.
  std::unique_ptr<FourwardPinsMirrorTestbed> testbed(
      new FourwardPinsMirrorTestbed(std::move(sut_switch),
                                    std::move(control_switch)));

  RETURN_IF_ERROR(testbed->StartBridge());
  return testbed;
}

FourwardPinsMirrorTestbed::~FourwardPinsMirrorTestbed() { StopBridge(); }

absl::Status FourwardPinsMirrorTestbed::StartBridge() {
  bridge_running_.store(true);

  std::string sut_address = sut_.ChassisName();
  std::string control_address = control_.ChassisName();

  ASSIGN_OR_RETURN(auto sut_gnmi_stub, sut_.CreateGnmiStub());
  ASSIGN_OR_RETURN(auto control_gnmi_stub, control_.CreateGnmiStub());

  // Shared ownership: each thread holds a reference, testbed holds the other.
  auto sut_gnmi = std::shared_ptr<gnmi::gNMI::StubInterface>(
      std::move(sut_gnmi_stub));
  auto control_gnmi = std::shared_ptr<gnmi::gNMI::StubInterface>(
      std::move(control_gnmi_stub));

  absl::Notification sut_to_control_ready;
  absl::Notification control_to_sut_ready;

  sut_to_control_thread_ = std::thread(
      &FourwardPinsMirrorTestbed::ForwardPackets, this, sut_address,
      control_address, std::ref(*sut_gnmi), std::ref(*control_gnmi),
      "SUT->Control", &sut_subscribe_ctx_, std::ref(sut_to_control_ready));
  control_to_sut_thread_ = std::thread(
      &FourwardPinsMirrorTestbed::ForwardPackets, this, control_address,
      sut_address, std::ref(*control_gnmi), std::ref(*sut_gnmi),
      "Control->SUT", &control_subscribe_ctx_, std::ref(control_to_sut_ready));

  constexpr absl::Duration kTimeout = absl::Seconds(10);
  if (!sut_to_control_ready.WaitForNotificationWithTimeout(kTimeout) ||
      !control_to_sut_ready.WaitForNotificationWithTimeout(kTimeout)) {
    StopBridge();
    return absl::DeadlineExceededError(
        "Packet bridge subscriptions did not become active within 10s");
  }

  LOG(INFO) << "Packet bridge started: " << sut_address << " <-> "
            << control_address;
  return absl::OkStatus();
}

void FourwardPinsMirrorTestbed::StopBridge() {
  if (!bridge_running_.load()) return;
  bridge_running_.store(false);
  {
    std::lock_guard<std::mutex> lock(bridge_contexts_mu_);
    if (sut_subscribe_ctx_) sut_subscribe_ctx_->TryCancel();
    if (control_subscribe_ctx_) control_subscribe_ctx_->TryCancel();
  }
  if (sut_to_control_thread_.joinable()) sut_to_control_thread_.join();
  if (control_to_sut_thread_.joinable()) control_to_sut_thread_.join();
  LOG(INFO) << "Packet bridge stopped";
}

void FourwardPinsMirrorTestbed::ForwardPackets(
    const std::string& from_address, const std::string& to_address,
    gnmi::gNMI::StubInterface& from_gnmi,
    gnmi::gNMI::StubInterface& to_gnmi,
    const std::string& direction_label, grpc::ClientContext** subscribe_ctx,
    absl::Notification& ready) {
  std::shared_ptr<grpc::Channel> from_channel =
      grpc::CreateChannel(from_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<Dataplane::StubInterface> from_stub =
      Dataplane::NewStub(from_channel);

  std::shared_ptr<grpc::Channel> to_channel =
      grpc::CreateChannel(to_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<Dataplane::StubInterface> to_stub =
      Dataplane::NewStub(to_channel);

  grpc::ClientContext ctx;
  {
    std::lock_guard<std::mutex> lock(bridge_contexts_mu_);
    *subscribe_ctx = &ctx;
  }
  SubscribeResultsRequest subscribe_request;
  std::unique_ptr<grpc::ClientReaderInterface<SubscribeResultsResponse>>
      reader = from_stub->SubscribeResults(&ctx, subscribe_request);

  SubscribeResultsResponse response;
  while (bridge_running_.load() && reader->Read(&response)) {
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

    // Forward the first possible outcome only — real hardware executes
    // exactly one alternative; forwarding all would duplicate packets for
    // nondeterministic programs.
    for (const PacketSet& outcome : result.possible_outcomes()) {
      for (const OutputPacket& output : outcome.packets()) {
        absl::StatusOr<std::string> iface_name =
            InterfaceNameForPort(from_gnmi, output.dataplane_egress_port());
        if (!iface_name.ok()) {
          LOG(WARNING) << direction_label
                       << ": failed to resolve interface name for port "
                       << output.dataplane_egress_port() << ": "
                       << iface_name.status();
          continue;
        }
        absl::StatusOr<uint32_t> target_port =
            PortForInterfaceName(to_gnmi, *iface_name);
        if (!target_port.ok()) {
          LOG(WARNING) << direction_label
                       << ": failed to resolve target port for interface '"
                       << *iface_name << "': " << target_port.status();
          continue;
        }

        InjectPacketRequest inject_request;
        inject_request.set_dataplane_ingress_port(*target_port);
        inject_request.set_payload(output.payload());

        grpc::ClientContext inject_ctx;
        InjectPacketResponse inject_response;
        grpc::Status inject_status = to_stub->InjectPacket(
            &inject_ctx, inject_request, &inject_response);
        if (!inject_status.ok()) {
          LOG(WARNING) << direction_label << ": InjectPacket failed: "
                       << inject_status.error_message();
        }
      }
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(bridge_contexts_mu_);
    *subscribe_ctx = nullptr;
  }
  reader->Finish();
  LOG(INFO) << direction_label << ": stopped";
}

}  // namespace dvaas
