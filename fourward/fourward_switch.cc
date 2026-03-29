#include "fourward/fourward_switch.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "fourward/fourward_server.h"
#include "grpcpp/channel.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"

namespace dvaas {

FourwardSwitch::FourwardSwitch(FourwardServer server,
                               std::string gnmi_address)
    : server_(std::move(server)),
      p4rt_address_(server_.Address()),
      gnmi_address_(std::move(gnmi_address)),
      device_id_(server_.DeviceId()) {}

absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
FourwardSwitch::CreateP4RuntimeStub() {
  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      p4rt_address_, grpc::InsecureChannelCredentials());
  return p4::v1::P4Runtime::NewStub(channel);
}

absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
FourwardSwitch::CreateGnmiStub() {
  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      gnmi_address_, grpc::InsecureChannelCredentials());
  return gnmi::gNMI::NewStub(channel);
}

}  // namespace dvaas
