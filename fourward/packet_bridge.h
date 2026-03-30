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

// PacketBridge: emulates back-to-back physical links between two 4ward
// P4RuntimeServer instances, routing by gNMI interface name.
//
// When the SUT outputs a packet on port X, the bridge:
//   1. Reads SUT's gNMI to find the interface name for port X.
//   2. Reads control switch's gNMI to find the port for that interface name.
//   3. Injects the packet on the control switch's port via unary InjectPacket.
//
// This lets the two switches have independent port numbering — they are
// connected by interface name (e.g. "Ethernet0" ↔ "Ethernet0"), not by
// dataplane port number.
//
//   DVaaS -> control_switch PacketOut -> bridge -> SUT ingress
//   SUT egress -> bridge -> control_switch ingress -> punt-all -> PacketIn -> DVaaS

#ifndef PINS_FOURWARD_PACKET_BRIDGE_H_
#define PINS_FOURWARD_PACKET_BRIDGE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/notification.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "grpcpp/client_context.h"

namespace dvaas {

// Bridges packet traffic between two 4ward instances, routing by gNMI
// interface name so the two switches can have independent port numbering.
//
// Usage:
//   PacketBridge bridge("localhost:9559", "localhost:9560",
//                       sut_gnmi_stub, control_gnmi_stub);
//   RETURN_IF_ERROR(bridge.Start());
//   // ... run DVaaS validation ...
//   bridge.Stop();
//
// The bridge spawns two threads (one per direction) that subscribe to
// SubscribeResults on each instance and forward output packets to the other.
class PacketBridge {
 public:
  PacketBridge(std::string sut_address, std::string control_address,
               std::shared_ptr<gnmi::gNMI::StubInterface> sut_gnmi_stub,
               std::shared_ptr<gnmi::gNMI::StubInterface> control_gnmi_stub);
  ~PacketBridge();

  // Starts the bridge. Returns OK once both subscriptions are active.
  absl::Status Start();

  // Stops the bridge and joins the forwarding threads.
  void Stop();

  // Returns the number of packets forwarded (both directions combined).
  int64_t PacketsForwarded() const { return packets_forwarded_.load(); }

  // Returns the number of packets that failed to inject.
  int64_t InjectFailures() const { return inject_failures_.load(); }

  // Looks up the gNMI interface name for a dataplane port on the given switch.
  // Uses the identity mapping: P4RT ID = dataplane port number.
  static absl::StatusOr<std::string> InterfaceNameForPort(
      gnmi::gNMI::StubInterface& gnmi_stub, uint32_t dataplane_port);

  // Looks up the dataplane port for a gNMI interface name on the given switch.
  // Uses the identity mapping: dataplane port number = P4RT ID.
  static absl::StatusOr<uint32_t> PortForInterfaceName(
      gnmi::gNMI::StubInterface& gnmi_stub, const std::string& interface_name);

 private:
  // Subscribes to SubscribeResults on `from` and forwards each output packet
  // to `to` via unary InjectPacket, routing by gNMI interface name.
  void ForwardLoop(const std::string& from_address,
                   const std::string& to_address,
                   gnmi::gNMI::StubInterface& from_gnmi,
                   gnmi::gNMI::StubInterface& to_gnmi,
                   const std::string& direction_label,
                   absl::Notification& ready);

  std::string sut_address_;
  std::string control_address_;
  std::shared_ptr<gnmi::gNMI::StubInterface> sut_gnmi_stub_;
  std::shared_ptr<gnmi::gNMI::StubInterface> control_gnmi_stub_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> packets_forwarded_{0};
  std::atomic<int64_t> inject_failures_{0};
  std::thread sut_to_control_;
  std::thread control_to_sut_;

  // ClientContext pointers for active SubscribeResults streams.
  // Stop() calls TryCancel() on these to unblock Read().
  std::mutex contexts_mu_;
  grpc::ClientContext* sut_subscribe_ctx_ = nullptr;
  grpc::ClientContext* control_subscribe_ctx_ = nullptr;

  // Signaled by each ForwardLoop when its SubscribeResults stream is active.
  absl::Notification sut_to_control_ready_;
  absl::Notification control_to_sut_ready_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_PACKET_BRIDGE_H_
