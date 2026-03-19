// Adapts a running 4ward server to the thinkit::Switch interface.
//
// P4Runtime is served by 4ward; gNMI is served by a separate stub (typically
// FakeGnmiService) since 4ward doesn't implement gNMI.

#ifndef PINS_FOURWARD_FOURWARD_SWITCH_H_
#define PINS_FOURWARD_FOURWARD_SWITCH_H_

#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "thinkit/switch.h"

namespace fourward {

class FourwardSwitch : public thinkit::Switch {
 public:
  // `p4rt_address`: "host:port" of the 4ward P4Runtime server.
  // `gnmi_address`: "host:port" of a gNMI service (e.g. FakeGnmiService).
  // `device_id`: P4Runtime device ID.
  FourwardSwitch(std::string p4rt_address, std::string gnmi_address,
                 uint32_t device_id);

  const std::string& ChassisName() override { return p4rt_address_; }
  uint32_t DeviceId() override { return device_id_; }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override;

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override;

 private:
  std::string p4rt_address_;
  std::string gnmi_address_;
  uint32_t device_id_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SWITCH_H_
