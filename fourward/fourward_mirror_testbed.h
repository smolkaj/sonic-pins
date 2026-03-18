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

// thinkit::MirrorTestbed backed by two 4ward P4RuntimeServer instances
// connected by a PacketBridge. Owns the full lifecycle: server subprocesses,
// fake gNMI, and the packet bridge are started on construction and torn down
// on destruction.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(auto testbed, FourwardMirrorTestbed::Start({
//       .binary_path = "path/to/p4runtime_server.jar",
//   }));
//   // testbed.Sut() and testbed.ControlSwitch() are ready for use.

#ifndef PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_
#define PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_

#include <string>

#include "absl/status/statusor.h"
#include "gutil/gutil/status.h"
#include "fourward/fourward_switch.h"
#include "fourward/packet_bridge.h"
#include "thinkit/bazel_test_environment.h"
#include "thinkit/mirror_testbed.h"

namespace fourward {

class FourwardMirrorTestbed : public thinkit::MirrorTestbed {
 public:
  struct Options {
    std::string binary_path;
  };

  // Starts two 4ward switches and connects them with a PacketBridge.
  static absl::StatusOr<std::unique_ptr<FourwardMirrorTestbed>> Start(
      Options options) {
    ASSIGN_OR_RETURN(auto sut, FourwardSwitch::Start({
        .binary_path = options.binary_path,
        .device_id = 1,
    }));
    ASSIGN_OR_RETURN(auto control, FourwardSwitch::Start({
        .binary_path = options.binary_path,
        .device_id = 2,
    }));
    auto bridge = std::make_unique<PacketBridge>(
        sut.P4rtAddress(), control.P4rtAddress());
    RETURN_IF_ERROR(bridge->Start());
    return std::unique_ptr<FourwardMirrorTestbed>(new FourwardMirrorTestbed(
        std::move(sut), std::move(control), std::move(bridge)));
  }

  FourwardMirrorTestbed(const FourwardMirrorTestbed&) = delete;
  FourwardMirrorTestbed& operator=(const FourwardMirrorTestbed&) = delete;

  thinkit::Switch& Sut() override { return sut_; }
  thinkit::Switch& ControlSwitch() override { return control_; }
  thinkit::TestEnvironment& Environment() override { return env_; }

  // P4Runtime address of the SUT (for DataplaneValidationBackend).
  const std::string& SutP4rtAddress() const { return sut_.P4rtAddress(); }

  PacketBridge& Bridge() { return *bridge_; }

 private:
  FourwardMirrorTestbed(FourwardSwitch sut, FourwardSwitch control,
                         std::unique_ptr<PacketBridge> bridge)
      : sut_(std::move(sut)),
        control_(std::move(control)),
        bridge_(std::move(bridge)),
        env_(/*mask_known_failures=*/false) {}

  FourwardSwitch sut_;
  FourwardSwitch control_;
  std::unique_ptr<PacketBridge> bridge_;
  thinkit::BazelTestEnvironment env_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_
