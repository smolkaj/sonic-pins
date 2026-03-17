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

// thinkit::Switch implementation backed by a 4ward P4RuntimeServer reachable
// via gRPC.

#ifndef PINS_FOURWARD_FOURWARD_SWITCH_H_
#define PINS_FOURWARD_FOURWARD_SWITCH_H_

#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "proto/gnmi/gnmi.grpc.pb.h"
#include "thinkit/switch.h"

namespace fourward {

// A thinkit::Switch backed by a 4ward P4RuntimeServer on localhost.
// P4Runtime and gNMI may be served on different addresses (4ward serves P4RT;
// a FakeGnmiServer serves gNMI).
class FourwardSwitch : public thinkit::Switch {
 public:
  FourwardSwitch(std::string p4rt_address, uint32_t device_id,
                 std::string gnmi_address)
      : p4rt_address_(std::move(p4rt_address)),
        device_id_(device_id),
        gnmi_address_(std::move(gnmi_address)) {}

  const std::string& ChassisName() override { return p4rt_address_; }
  uint32_t DeviceId() override { return device_id_; }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override {
    auto channel = grpc::CreateChannel(p4rt_address_,
                                       grpc::InsecureChannelCredentials());
    return p4::v1::P4Runtime::NewStub(channel);
  }

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override {
    auto channel = grpc::CreateChannel(gnmi_address_,
                                       grpc::InsecureChannelCredentials());
    return gnmi::gNMI::NewStub(channel);
  }

 private:
  std::string p4rt_address_;
  uint32_t device_id_;
  std::string gnmi_address_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SWITCH_H_
