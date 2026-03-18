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

// thinkit::Switch backed by a 4ward P4RuntimeServer subprocess and an
// in-process fake gNMI server. Owns the subprocess lifecycle — the server
// is killed on destruction.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(auto sw, FourwardSwitch::Start({.device_id = 1}));
//   // sw is a fully functional thinkit::Switch with P4Runtime + gNMI.

#ifndef PINS_FOURWARD_FOURWARD_SWITCH_H_
#define PINS_FOURWARD_FOURWARD_SWITCH_H_

#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "gutil/gutil/status.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_server.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "proto/gnmi/gnmi.grpc.pb.h"
#include "thinkit/switch.h"

namespace fourward {

// A thinkit::Switch that owns a 4ward P4RuntimeServer subprocess and an
// in-process FakeGnmiServer. Both are started on random ports and torn down
// on destruction.
class FourwardSwitch : public thinkit::Switch {
 public:
  using Options = FourwardServer::Options;

  // Starts a 4ward subprocess and fake gNMI server.
  static absl::StatusOr<FourwardSwitch> Start(Options options) {
    ASSIGN_OR_RETURN(auto server, FourwardServer::Start(std::move(options)));
    auto gnmi = std::make_unique<FakeGnmiServer>();
    return FourwardSwitch(std::move(server), std::move(gnmi));
  }

  FourwardSwitch(const FourwardSwitch&) = delete;
  FourwardSwitch& operator=(const FourwardSwitch&) = delete;
  FourwardSwitch(FourwardSwitch&&) = default;
  FourwardSwitch& operator=(FourwardSwitch&&) = default;

  const std::string& ChassisName() override { return server_.Address(); }
  uint32_t DeviceId() override { return server_.DeviceId(); }

  // P4Runtime address (for direct gRPC use outside the thinkit interface).
  const std::string& P4rtAddress() const { return server_.Address(); }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override {
    auto channel = grpc::CreateChannel(server_.Address(),
                                       grpc::InsecureChannelCredentials());
    return p4::v1::P4Runtime::NewStub(channel);
  }

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override {
    auto channel = grpc::CreateChannel(gnmi_->address,
                                       grpc::InsecureChannelCredentials());
    return gnmi::gNMI::NewStub(channel);
  }

 private:
  FourwardSwitch(FourwardServer server,
                 std::unique_ptr<FakeGnmiServer> gnmi)
      : server_(std::move(server)), gnmi_(std::move(gnmi)) {}

  FourwardServer server_;
  std::unique_ptr<FakeGnmiServer> gnmi_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SWITCH_H_
