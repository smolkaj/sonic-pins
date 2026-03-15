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

// thinkit::Switch and thinkit::MirrorTestbed implementations backed by 4ward
// P4RuntimeServer instances reachable via gRPC.

#ifndef PINS_FOURWARD_FOURWARD_SWITCH_H_
#define PINS_FOURWARD_FOURWARD_SWITCH_H_

#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "grpcpp/channel.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "proto/gnmi/gnmi.grpc.pb.h"
#include "thinkit/mirror_testbed.h"
#include "thinkit/switch.h"
#include "thinkit/test_environment.h"

namespace fourward {

// A thinkit::Switch backed by a 4ward P4RuntimeServer on localhost.
class FourwardSwitch : public thinkit::Switch {
 public:
  FourwardSwitch(std::string address, uint32_t device_id)
      : address_(std::move(address)), device_id_(device_id) {}

  const std::string& ChassisName() override { return address_; }
  uint32_t DeviceId() override { return device_id_; }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override {
    auto channel = grpc::CreateChannel(address_,
                                       grpc::InsecureChannelCredentials());
    return p4::v1::P4Runtime::NewStub(channel);
  }

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override {
    auto channel = grpc::CreateChannel(address_,
                                       grpc::InsecureChannelCredentials());
    return gnmi::gNMI::NewStub(channel);
  }

 private:
  std::string address_;
  uint32_t device_id_;
};

// Minimal thinkit::TestEnvironment that writes artifacts to /tmp.
class FourwardTestEnvironment : public thinkit::TestEnvironment {
 public:
  absl::Status StoreTestArtifact(absl::string_view filename,
                                 absl::string_view contents) override {
    // Log for visibility; a real implementation would write to disk.
    LOG(INFO) << "Test artifact '" << filename << "': " << contents.size()
              << " bytes";
    return absl::OkStatus();
  }

  absl::Status AppendToTestArtifact(absl::string_view filename,
                                    absl::string_view contents) override {
    return absl::OkStatus();
  }

  bool MaskKnownFailures() override { return false; }
};

// A thinkit::MirrorTestbed with two 4ward P4RuntimeServer instances.
class FourwardMirrorTestbed : public thinkit::MirrorTestbed {
 public:
  FourwardMirrorTestbed(std::string sut_address, uint32_t sut_device_id,
                        std::string control_address,
                        uint32_t control_device_id)
      : sut_(std::move(sut_address), sut_device_id),
        control_(std::move(control_address), control_device_id) {}

  thinkit::Switch& Sut() override { return sut_; }
  thinkit::Switch& ControlSwitch() override { return control_; }
  thinkit::TestEnvironment& Environment() override { return env_; }

 private:
  FourwardSwitch sut_;
  FourwardSwitch control_;
  FourwardTestEnvironment env_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SWITCH_H_
