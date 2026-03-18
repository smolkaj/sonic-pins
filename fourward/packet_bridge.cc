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

#include <memory>
#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "fourward/dataplane.grpc.pb.h"
#include "fourward/dataplane.pb.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"

namespace fourward {

PacketBridge::PacketBridge(std::string sut_address,
                           std::string control_address)
    : sut_address_(std::move(sut_address)),
      control_address_(std::move(control_address)) {}

PacketBridge::~PacketBridge() { Stop(); }

absl::Status PacketBridge::Start() {
  if (running_.load()) {
    return absl::FailedPreconditionError("PacketBridge already running");
  }
  running_.store(true);

  // Spawn forwarding threads for both directions.
  sut_to_control_ = std::thread(&PacketBridge::ForwardLoop, this,
                                sut_address_, control_address_,
                                "SUT→Control",
                                std::ref(sut_to_control_ready_));
  control_to_sut_ = std::thread(&PacketBridge::ForwardLoop, this,
                                control_address_, sut_address_,
                                "Control→SUT",
                                std::ref(control_to_sut_ready_));
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
  LOG(INFO) << "PacketBridge stopped. Total packets forwarded: "
            << packets_forwarded_.load();
}

void PacketBridge::ForwardLoop(const std::string& from_address,
                               const std::string& to_address,
                               const std::string& direction_label,
                               absl::Notification& ready) {
  // Create channels and stubs.
  auto from_channel =
      grpc::CreateChannel(from_address, grpc::InsecureChannelCredentials());
  auto from_stub = dataplane::Dataplane::NewStub(from_channel);

  auto to_channel =
      grpc::CreateChannel(to_address, grpc::InsecureChannelCredentials());
  auto to_stub = dataplane::Dataplane::NewStub(to_channel);

  // Subscribe to SubscribeResults on the "from" instance.
  grpc::ClientContext subscribe_ctx;
  {
    std::lock_guard<std::mutex> lock(contexts_mu_);
    if (from_address == sut_address_)
      sut_subscribe_ctx_ = &subscribe_ctx;
    else
      control_subscribe_ctx_ = &subscribe_ctx;
  }
  dataplane::SubscribeResultsRequest subscribe_request;
  auto reader = from_stub->SubscribeResults(&subscribe_ctx, subscribe_request);

  LOG(INFO) << direction_label << ": subscribed to " << from_address;

  dataplane::SubscribeResultsResponse response;
  while (running_.load() && reader->Read(&response)) {
    if (response.has_active()) {
      LOG(INFO) << direction_label << ": subscription active";
      ready.Notify();
      continue;
    }

    if (!response.has_result()) continue;
    const auto& result = response.result();

    // Forward each output packet to the other instance.
    for (const auto& output : result.output_packets()) {
      dataplane::InjectPacketRequest inject_request;
      auto* packet = inject_request.mutable_packet();
      // The output's egress port becomes the input's ingress port on the
      // other instance (simulating a back-to-back physical link).
      packet->set_ingress_port(output.egress_port());
      packet->set_payload(output.payload());

      grpc::ClientContext inject_ctx;
      dataplane::InjectPacketResponse inject_response;
      auto status =
          to_stub->InjectPacket(&inject_ctx, inject_request, &inject_response);
      if (status.ok()) {
        packets_forwarded_.fetch_add(1);
        VLOG(1) << direction_label << ": forwarded packet on port "
                << output.egress_port();
      } else {
        LOG(WARNING) << direction_label
                     << ": failed to inject packet on port "
                     << output.egress_port() << ": " << status.error_message();
      }
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
  if (running_.load()) {
    auto status = reader->Finish();
    LOG(WARNING) << direction_label << ": subscription ended: "
                 << status.error_message();
  } else {
    reader->Finish();
    LOG(INFO) << direction_label << ": stopped";
  }
}

}  // namespace fourward
