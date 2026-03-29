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
#include "fourward/fourward_server.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "thinkit/switch.h"

namespace dvaas {

class FourwardSwitch : public thinkit::Switch {
 public:
  // `server`: a running FourwardServer (taken by move). The P4Runtime address
  //   is derived from `server.Address()`.
  // `gnmi_address`: "host:port" of a gNMI service (e.g. FakeGnmiService).
  FourwardSwitch(FourwardServer server, std::string gnmi_address);

  const std::string& ChassisName() override { return p4rt_address_; }
  uint32_t DeviceId() override { return device_id_; }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override;

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override;

 private:
  FourwardServer server_;
  std::string p4rt_address_;
  std::string gnmi_address_;
  uint32_t device_id_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_SWITCH_H_
