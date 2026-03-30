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

#include "fourward/packet_bridge.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "gutil/status.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.pb.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "re2/re2.h"

namespace dvaas {

using fourward::dataplane::Dataplane;
using fourward::dataplane::InjectPacketRequest;
using fourward::dataplane::InjectPacketResponse;
using fourward::dataplane::OutputPacket;
using fourward::dataplane::PacketSet;
using fourward::dataplane::ProcessPacketResult;
using fourward::dataplane::SubscribeResultsRequest;
using fourward::dataplane::SubscribeResultsResponse;

PacketBridge::PacketBridge(
    std::string sut_address, std::string control_address,
    std::shared_ptr<gnmi::gNMI::StubInterface> sut_gnmi_stub,
    std::shared_ptr<gnmi::gNMI::StubInterface> control_gnmi_stub)
    : sut_address_(std::move(sut_address)),
      control_address_(std::move(control_address)),
      sut_gnmi_stub_(std::move(sut_gnmi_stub)),
      control_gnmi_stub_(std::move(control_gnmi_stub)) {}

PacketBridge::~PacketBridge() { Stop(); }

absl::Status PacketBridge::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return absl::FailedPreconditionError("PacketBridge already running");
  }

  // Spawn forwarding threads for both directions.
  sut_to_control_ = std::thread(
      &PacketBridge::ForwardLoop, this, sut_address_, control_address_,
      std::ref(*sut_gnmi_stub_), std::ref(*control_gnmi_stub_),
      "SUT->Control", std::ref(sut_to_control_ready_));
  control_to_sut_ = std::thread(
      &PacketBridge::ForwardLoop, this, control_address_, sut_address_,
      std::ref(*control_gnmi_stub_), std::ref(*sut_gnmi_stub_),
      "Control->SUT", std::ref(control_to_sut_ready_));
  // Block until both subscriptions are active.
  constexpr absl::Duration kTimeout = absl::Seconds(10);
  if (!sut_to_control_ready_.WaitForNotificationWithTimeout(kTimeout) ||
      !control_to_sut_ready_.WaitForNotificationWithTimeout(kTimeout)) {
    Stop();
    return absl::DeadlineExceededError(
        "PacketBridge subscriptions did not become active within 10s");
  }

  LOG(INFO) << "PacketBridge started: " << sut_address_ << " <-> "
            << control_address_;
  return absl::OkStatus();
}

void PacketBridge::Stop() {
  if (!running_.load()) return;
  running_.store(false);
  // Cancel active SubscribeResults streams to unblock Read().
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (sut_subscribe_ctx_) sut_subscribe_ctx_->TryCancel();
    if (control_subscribe_ctx_) control_subscribe_ctx_->TryCancel();
  }
  if (sut_to_control_.joinable()) sut_to_control_.join();
  if (control_to_sut_.joinable()) control_to_sut_.join();
  LOG(INFO) << "PacketBridge stopped. Forwarded: " << packets_forwarded_.load()
            << ", inject failures: " << inject_failures_.load();
}

// Matches interface entries in FakeGnmiService state JSON, e.g.:
//   "state":{"name":"Ethernet0",...,"openconfig-p4rt:id":1}
static const re2::RE2& InterfaceStatePattern() {
  static const re2::RE2* pattern = new re2::RE2(
      R"re("state":\{"name":"([^"]+)"[^}]*"openconfig-p4rt:id":(\d+)\})re");
  return *pattern;
}

// Fetches gNMI state JSON from the given stub.
static absl::StatusOr<std::string> GetGnmiStateJson(
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

absl::StatusOr<std::string> PacketBridge::InterfaceNameForPort(
    gnmi::gNMI::StubInterface& gnmi_stub, uint32_t dataplane_port) {
  ASSIGN_OR_RETURN(std::string json, GetGnmiStateJson(gnmi_stub));

  // P4RT ID = dataplane port (identity mapping per design).
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

absl::StatusOr<uint32_t> PacketBridge::PortForInterfaceName(
    gnmi::gNMI::StubInterface& gnmi_stub,
    const std::string& interface_name) {
  ASSIGN_OR_RETURN(std::string json, GetGnmiStateJson(gnmi_stub));

  re2::StringPiece input(json);
  std::string name;
  int id;
  while (RE2::FindAndConsume(&input, InterfaceStatePattern(), &name, &id)) {
    if (name == interface_name) {
      // P4RT ID = dataplane port (identity mapping per design).
      return static_cast<uint32_t>(id);
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("No interface named '%s'", interface_name));
}

void PacketBridge::ForwardLoop(const std::string& from_address,
                               const std::string& to_address,
                               gnmi::gNMI::StubInterface& from_gnmi,
                               gnmi::gNMI::StubInterface& to_gnmi,
                               const std::string& direction_label,
                               absl::Notification& ready) {
  // Create channels and stubs.
  std::shared_ptr<grpc::Channel> from_channel =
      grpc::CreateChannel(from_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<Dataplane::StubInterface> from_stub =
      Dataplane::NewStub(from_channel);

  std::shared_ptr<grpc::Channel> to_channel =
      grpc::CreateChannel(to_address, grpc::InsecureChannelCredentials());
  std::unique_ptr<Dataplane::StubInterface> to_stub =
      Dataplane::NewStub(to_channel);

  // Subscribe to SubscribeResults on the "from" instance.
  grpc::ClientContext subscribe_ctx;
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (from_address == sut_address_)
      sut_subscribe_ctx_ = &subscribe_ctx;
    else
      control_subscribe_ctx_ = &subscribe_ctx;
  }
  SubscribeResultsRequest subscribe_request;
  std::unique_ptr<grpc::ClientReaderInterface<SubscribeResultsResponse>>
      reader = from_stub->SubscribeResults(&subscribe_ctx, subscribe_request);

  LOG(INFO) << direction_label << ": subscribed to " << from_address;

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
        // Resolve interface name on the source switch, then find the
        // corresponding port on the target switch.
        absl::StatusOr<std::string> iface_name =
            InterfaceNameForPort(from_gnmi, output.dataplane_egress_port());
        if (!iface_name.ok()) {
          LOG(WARNING) << direction_label
                       << ": failed to resolve interface name for port "
                       << output.dataplane_egress_port() << ": "
                       << iface_name.status();
          inject_failures_.fetch_add(1);
          continue;
        }
        absl::StatusOr<uint32_t> target_port =
            PortForInterfaceName(to_gnmi, *iface_name);
        if (!target_port.ok()) {
          LOG(WARNING) << direction_label
                       << ": failed to resolve target port for interface '"
                       << *iface_name << "': " << target_port.status();
          inject_failures_.fetch_add(1);
          continue;
        }

        // Inject via unary InjectPacket to avoid holding the write lock
        // across packets.
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
    if (from_address == sut_address_)
      sut_subscribe_ctx_ = nullptr;
    else
      control_subscribe_ctx_ = nullptr;
  }
  reader->Finish();
  LOG(INFO) << direction_label << ": stopped";
}

}  // namespace dvaas
