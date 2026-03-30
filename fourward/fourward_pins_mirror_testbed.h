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

// thinkit::MirrorTestbed backed by two FourwardPinsSwitch instances (each
// owning a 4ward P4Runtime server and a fake gNMI server) with integrated
// packet bridging that routes by interface name.

#ifndef PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_
#define PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/notification.h"
#include "fourward/fourward_pins_switch.h"
#include "grpcpp/client_context.h"
#include "thinkit/bazel_test_environment.h"
#include "thinkit/mirror_testbed.h"

namespace dvaas {

// A thinkit::MirrorTestbed with two FourwardPinsSwitch instances and integrated
// packet bridging. Replaces the standalone PacketBridge with direct
// FakeGnmiService lookups for port resolution.
class FourwardPinsMirrorTestbed : public thinkit::MirrorTestbed {
 public:
  // Creates a testbed with two FourwardPinsSwitch instances. Call
  // StartBridge() after loading pipelines to enable packet forwarding.
  static absl::StatusOr<std::unique_ptr<FourwardPinsMirrorTestbed>> Create(
      uint32_t sut_device_id = 1, uint32_t control_device_id = 2);

  ~FourwardPinsMirrorTestbed();

  thinkit::Switch& Sut() override { return sut_; }
  thinkit::Switch& ControlSwitch() override { return control_; }
  thinkit::TestEnvironment& Environment() override { return env_; }

  // Starts packet bridging between the two switches.
  absl::Status StartBridge();

  // Stops packet bridging and joins the forwarding threads.
  void StopBridge();

  // Returns the number of packets forwarded (both directions combined).
  int64_t PacketsForwarded() const { return packets_forwarded_.load(); }

  // Returns the number of packets that failed to inject.
  int64_t InjectFailures() const { return inject_failures_.load(); }

 private:
  explicit FourwardPinsMirrorTestbed(FourwardPinsSwitch sut,
                                     FourwardPinsSwitch control)
      : sut_(std::move(sut)),
        control_(std::move(control)),
        env_(/*mask_known_failures=*/false) {}

  // Resolves a dataplane port on `from_switch` to the corresponding port on
  // `to_switch` via matching interface names in their FakeGnmiService.
  static absl::StatusOr<uint32_t> ResolveTargetPort(
      FourwardPinsSwitch& from_switch, FourwardPinsSwitch& to_switch,
      uint32_t egress_port);

  // Subscribes to SubscribeResults on `from_switch` and forwards each output
  // packet to `to_switch`, routing by interface name.
  void ForwardLoop(FourwardPinsSwitch& from_switch,
                   FourwardPinsSwitch& to_switch,
                   const std::string& direction_label,
                   absl::Notification& ready);

  FourwardPinsSwitch sut_;
  FourwardPinsSwitch control_;
  thinkit::BazelTestEnvironment env_;

  // Bridge state.
  std::atomic<bool> running_{false};
  std::atomic<int64_t> packets_forwarded_{0};
  std::atomic<int64_t> inject_failures_{0};
  std::thread sut_to_control_;
  std::thread control_to_sut_;

  // ClientContext pointers for active SubscribeResults streams.
  // StopBridge() calls TryCancel() on these to unblock Read().
  std::mutex contexts_mu_;
  grpc::ClientContext* sut_subscribe_ctx_ = nullptr;
  grpc::ClientContext* control_subscribe_ctx_ = nullptr;

  // Signaled by each ForwardLoop when its SubscribeResults stream is active.
  absl::Notification sut_to_control_ready_;
  absl::Notification control_to_sut_ready_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_
