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

// thinkit::MirrorTestbed backed by two 4ward P4RuntimeServer instances and
// in-process fake gNMI servers.

#ifndef PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_
#define PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_

#include <cstdint>
#include <string>

#include "fourward/fourward_switch.h"
#include "thinkit/bazel_test_environment.h"
#include "thinkit/mirror_testbed.h"

namespace fourward {

// A thinkit::MirrorTestbed with two 4ward P4RuntimeServer instances and
// in-process fake gNMI servers.
class FourwardMirrorTestbed : public thinkit::MirrorTestbed {
 public:
  FourwardMirrorTestbed(std::string sut_p4rt_address, uint32_t sut_device_id,
                        std::string sut_gnmi_address,
                        std::string control_p4rt_address,
                        uint32_t control_device_id,
                        std::string control_gnmi_address)
      : sut_(std::move(sut_p4rt_address), sut_device_id,
             std::move(sut_gnmi_address)),
        control_(std::move(control_p4rt_address), control_device_id,
                 std::move(control_gnmi_address)),
        env_(/*mask_known_failures=*/false) {}

  thinkit::Switch& Sut() override { return sut_; }
  thinkit::Switch& ControlSwitch() override { return control_; }
  thinkit::TestEnvironment& Environment() override { return env_; }

 private:
  FourwardSwitch sut_;
  FourwardSwitch control_;
  thinkit::BazelTestEnvironment env_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_MIRROR_TESTBED_H_
