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
// owning a 4ward P4Runtime server and a fake gNMI server) and an integrated
// packet bridge connecting them.
//
// The packet bridge emulates back-to-back physical links, routing by gNMI
// interface name so the two switches can have independent port numbering.
// When either instance outputs a packet, the bridge resolves the egress port
// to an interface name via gNMI, then finds the corresponding port on the
// other instance and injects the packet there.
//
//   DVaaS -> control_switch PacketOut -> bridge -> SUT ingress
//   SUT egress -> bridge -> control_switch ingress -> punt-all -> PacketIn -> DVaaS

#ifndef PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_
#define PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "absl/status/statusor.h"
#include "fourward/fourward_pins_switch.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "grpcpp/client_context.h"
#include "thinkit/bazel_test_environment.h"
#include "thinkit/mirror_testbed.h"

namespace dvaas {

// A thinkit::MirrorTestbed with two FourwardPinsSwitch instances and a
// packet bridge connecting them.
//
// The bridge starts automatically on creation and stops when the testbed is
// destroyed.
class FourwardPinsMirrorTestbed : public thinkit::MirrorTestbed {
 public:
  // Creates a fully wired testbed: two FourwardPinsSwitch instances and a
  // running packet bridge between them.
  static absl::StatusOr<std::unique_ptr<FourwardPinsMirrorTestbed>> Create(
      uint32_t sut_device_id = 1, uint32_t control_device_id = 2);

  ~FourwardPinsMirrorTestbed();

  FourwardPinsMirrorTestbed(const FourwardPinsMirrorTestbed&) = delete;
  FourwardPinsMirrorTestbed& operator=(const FourwardPinsMirrorTestbed&) =
      delete;

  thinkit::Switch& Sut() override { return sut_; }
  thinkit::Switch& ControlSwitch() override { return control_; }
  thinkit::TestEnvironment& Environment() override { return env_; }

 private:
  FourwardPinsMirrorTestbed(FourwardPinsSwitch sut, FourwardPinsSwitch control)
      : sut_(std::move(sut)),
        control_(std::move(control)),
        env_(/*mask_known_failures=*/false) {}

  absl::Status StartBridge();
  void StopBridge();

  // Subscribes to SubscribeResults on `from_address` and forwards each output
  // packet to `to_address`, routing by gNMI interface name. Stores its
  // subscribe context in `*subscribe_ctx` so StopBridge() can cancel it.
  // Notifies `ready` once the subscription is active.
  void ForwardPackets(const std::string& from_address,
                      const std::string& to_address,
                      gnmi::gNMI::StubInterface& from_gnmi,
                      gnmi::gNMI::StubInterface& to_gnmi,
                      const std::string& direction_label,
                      grpc::ClientContext** subscribe_ctx,
                      absl::Notification& ready);

  FourwardPinsSwitch sut_;
  FourwardPinsSwitch control_;
  thinkit::BazelTestEnvironment env_;

  // Packet bridge state.
  std::atomic<bool> bridge_running_{false};
  std::thread sut_to_control_thread_;
  std::thread control_to_sut_thread_;
  std::mutex bridge_contexts_mu_;
  grpc::ClientContext* sut_subscribe_ctx_ = nullptr;
  grpc::ClientContext* control_subscribe_ctx_ = nullptr;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_PINS_MIRROR_TESTBED_H_
