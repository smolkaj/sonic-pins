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

#include "fourward/fourward_pins_mirror_testbed.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "fourward/fourward_pins_switch.h"
#include "fourward/packet_bridge.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "gutil/status.h"

namespace dvaas {

absl::StatusOr<std::unique_ptr<FourwardPinsMirrorTestbed>>
FourwardPinsMirrorTestbed::Create(uint32_t sut_device_id,
                                  uint32_t control_device_id) {
  ASSIGN_OR_RETURN(
      FourwardPinsSwitch sut_switch,
      FourwardPinsSwitch::Create({.device_id = sut_device_id}));
  ASSIGN_OR_RETURN(
      FourwardPinsSwitch control_switch,
      FourwardPinsSwitch::Create({.device_id = control_device_id}));

  std::string sut_address = sut_switch.ChassisName();
  std::string control_address = control_switch.ChassisName();

  ASSIGN_OR_RETURN(auto sut_gnmi_stub, sut_switch.CreateGnmiStub());
  ASSIGN_OR_RETURN(auto control_gnmi_stub, control_switch.CreateGnmiStub());

  std::unique_ptr<PacketBridge> bridge = std::make_unique<PacketBridge>(
      sut_address, control_address,
      std::shared_ptr<gnmi::gNMI::StubInterface>(std::move(sut_gnmi_stub)),
      std::shared_ptr<gnmi::gNMI::StubInterface>(
          std::move(control_gnmi_stub)));

  // Can't use make_unique due to private constructor.
  return std::unique_ptr<FourwardPinsMirrorTestbed>(
      new FourwardPinsMirrorTestbed(std::move(sut_switch),
                                    std::move(control_switch),
                                    std::move(bridge)));
}

}  // namespace dvaas
