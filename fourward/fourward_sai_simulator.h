// Test-ready SAI P4 simulator backed by 4ward.
//
// Example:
//
//   ASSERT_OK_AND_ASSIGN(auto simulator,
//       FourwardSaiSimulator::Create(sai::Instantiation::kMiddleblock));
//   ASSERT_OK(pdpi::InstallIrEntities(simulator.p4rt(), entries));
//   ASSERT_OK(simulator.InstallAuxEntries());
//
//   EXPECT_THAT(simulator.dataplane().InjectPacket({
//       .ingress_port = fourward::DataplanePort{.port = 0},
//       .payload = packet,
//   }), IsOkAndHolds(ForwardsTo(1)));

#ifndef PINS_FOURWARD_FOURWARD_SAI_SIMULATOR_H_
#define PINS_FOURWARD_FOURWARD_SAI_SIMULATOR_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "p4runtime_cc/fourward_server.h"
#include "p4_infra/p4_runtime/p4_runtime_session.h"
#include "p4runtime_cc/dataplane_client.h"
#include "p4/v1/p4runtime.pb.h"
#include "sai_p4/instantiations/google/instantiations.h"

namespace dvaas {

// Test-ready SAI P4 simulator backed by 4ward. Abstracts server lifecycle
// and gRPC boilerplate, exposing a P4RuntimeSession for control-plane
// operations and a DataplaneClient for packet injection. For PINS test
// infrastructure (thinkit::Switch), use FourwardPinsSwitch instead.
class FourwardSaiSimulator {
 public:
  // Starts a 4ward server, loads the given SAI pipeline, and establishes
  // a P4Runtime session with master arbitration.
  static absl::StatusOr<FourwardSaiSimulator> Create(
      sai::Instantiation instantiation,
      fourward::FourwardServerOptions options = {});
  static absl::StatusOr<FourwardSaiSimulator> Create(
      p4::v1::ForwardingPipelineConfig config,
      fourward::FourwardServerOptions options = {});

  ~FourwardSaiSimulator();
  FourwardSaiSimulator(FourwardSaiSimulator&&);
  FourwardSaiSimulator& operator=(FourwardSaiSimulator&&);
  FourwardSaiSimulator(const FourwardSaiSimulator&) = delete;
  FourwardSaiSimulator& operator=(const FourwardSaiSimulator&) = delete;

  p4_runtime::P4RuntimeSession& p4rt();
  fourward::DataplaneClient& dataplane();

  // Installs auxiliary entries required by the loaded SAI pipeline
  // (clone sessions, punt entries, etc.). Call after installing table
  // entries — aux entries depend on what's already installed.
  absl::Status InstallAuxEntries();

 private:
  fourward::FourwardServer server_;
  std::unique_ptr<p4_runtime::P4RuntimeSession> p4rt_;
  fourward::DataplaneClient dataplane_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_SAI_SIMULATOR_H_
