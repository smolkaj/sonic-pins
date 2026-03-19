#include "fourward/fourward_switch.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "grpcpp/channel.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"

namespace fourward {

FourwardSwitch::FourwardSwitch(std::string p4rt_address,
                               std::string gnmi_address, uint32_t device_id)
    : p4rt_address_(std::move(p4rt_address)),
      gnmi_address_(std::move(gnmi_address)),
      device_id_(device_id) {}

absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
FourwardSwitch::CreateP4RuntimeStub() {
  auto channel = grpc::CreateChannel(p4rt_address_,
                                     grpc::InsecureChannelCredentials());
  return p4::v1::P4Runtime::NewStub(channel);
}

absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
FourwardSwitch::CreateGnmiStub() {
  auto channel = grpc::CreateChannel(gnmi_address_,
                                     grpc::InsecureChannelCredentials());
  return gnmi::gNMI::NewStub(channel);
}

}  // namespace fourward
