// Simulated PINS switch backed by a 4ward P4Runtime server.
//
// Starts a 4ward server subprocess, an in-process fake gNMI server, and a
// pre-packet hook that automatically installs auxiliary P4 entries — the
// "control plane glue" that makes the simulated switch behave like a real PINS
// switch. Currently installs PRE clone sessions and ingress_clone_table entries
// for punting; VLAN and L3 admit entries are planned.

#ifndef PINS_FOURWARD_FOURWARD_PINS_SWITCH_H_
#define PINS_FOURWARD_FOURWARD_PINS_SWITCH_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "dataplane.grpc.pb.h"
#include "grpcpp/channel.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_server.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "thinkit/switch.h"

namespace dvaas {

// Options for FourwardPinsSwitch::Create.
struct FourwardPinsSwitchOptions {
  uint32_t device_id = 1;
  std::vector<FakeInterface> interfaces =
      FakeGnmiService::DefaultInterfaces();
  // When true (default), a pre-packet hook automatically installs auxiliary
  // P4 entries (PRE clone sessions, ingress_clone_table entries) before each
  // packet. Set to false for testing the "without auxiliary entries" case.
  bool enable_auxiliary_entries = true;
};

class FourwardPinsSwitch : public thinkit::Switch {
 public:
  // Creates a FourwardPinsSwitch by starting a 4ward server subprocess, a fake
  // gNMI server, and an auxiliary entry hook that transparently installs PRE
  // clone sessions and other auxiliary entries before each packet.
  static absl::StatusOr<FourwardPinsSwitch> Create(
      FourwardPinsSwitchOptions options = {});

  ~FourwardPinsSwitch();

  FourwardPinsSwitch(FourwardPinsSwitch&&);
  FourwardPinsSwitch& operator=(FourwardPinsSwitch&&);
  FourwardPinsSwitch(const FourwardPinsSwitch&) = delete;
  FourwardPinsSwitch& operator=(const FourwardPinsSwitch&) = delete;

  const std::string& ChassisName() override { return server_.Address(); }
  uint32_t DeviceId() override { return server_.DeviceId(); }

  absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
  CreateP4RuntimeStub() override;

  absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
  CreateGnmiStub() override;

  // Creates a stub for the 4ward Dataplane service (packet injection).
  std::unique_ptr<fourward::dataplane::Dataplane::Stub> CreateDataplaneStub();

 private:
  FourwardPinsSwitch(FourwardServer server,
                     std::unique_ptr<FakeGnmiServer> gnmi_server);

  FourwardServer server_;
  // Cached gRPC channel to the 4ward server (shared by P4Runtime + Dataplane
  // stubs). gRPC channels are heavyweight; reuse avoids redundant connections.
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<FakeGnmiServer> gnmi_server_;

  // Pre-packet hook state, behind a pointer so FourwardPinsSwitch stays
  // movable (grpc::ClientContext is not movable). Defined in the .cc file.
  struct HookState;
  static void RunHookLoop(HookState& hook);
  std::unique_ptr<HookState> hook_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_PINS_SWITCH_H_
