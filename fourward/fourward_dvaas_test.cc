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

// End-to-end test: runs upstream DVaaS against two 4ward P4RuntimeServer
// instances, connected by a PacketBridge, with fake gNMI servers for port
// discovery.
//
// The test starts the 4ward servers as subprocesses, compiles/loads the SAI
// middleblock pipeline, and runs DVaaS validation — all self-contained. The
// only external dependency is the 4ward server binary, provided as a flag
// (will become a Bazel data dep).

#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/log.h"
#include "dvaas/dataplane_validation.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_dataplane_validation_backend.h"
#include "fourward/fourward_server.h"
#include "fourward/fourward_mirror_testbed.h"
#include "fourward/packet_bridge.h"
#include "gtest/gtest.h"
#include "grpcpp/security/credentials.h"
#include "gutil/gutil/io.h"
#include "gutil/gutil/status_matchers.h"
#include "p4_infra/p4_pdpi/p4_runtime_session.h"
#include "sai_p4/instantiations/google/instantiations.h"
#include "sai_p4/instantiations/google/sai_p4info.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

ABSL_FLAG(std::string, server_binary, "",
          "Path to the 4ward P4RuntimeServer binary.");
ABSL_FLAG(std::string, pipeline, "",
          "Path to compiled ForwardingPipelineConfig binary.");

namespace fourward {
namespace {

using ::gutil::IsOk;

TEST(FourwardDvaasTest, UpstreamDvaasValidation) {
  std::string binary = absl::GetFlag(FLAGS_server_binary);
  ASSERT_FALSE(binary.empty())
      << "Must set --server_binary to the path of the 4ward "
         "P4RuntimeServer binary.";

  std::string pipeline_path = absl::GetFlag(FLAGS_pipeline);
  ASSERT_FALSE(pipeline_path.empty())
      << "Must set --pipeline to the path of a compiled "
         "ForwardingPipelineConfig binary (.binpb).";

  // Start two 4ward P4Runtime servers on random ports.
  ASSERT_OK_AND_ASSIGN(auto sut_server, FourwardServer::Start({
      .binary_path = binary,
      .device_id = 1,
  }));
  ASSERT_OK_AND_ASSIGN(auto control_server, FourwardServer::Start({
      .binary_path = binary,
      .device_id = 2,
  }));

  LOG(INFO) << "SUT: " << sut_server.Address()
            << " (device " << sut_server.DeviceId() << ")";
  LOG(INFO) << "Control: " << control_server.Address()
            << " (device " << control_server.DeviceId() << ")";

  // Load the compiled ForwardingPipelineConfig binary proto.
  ASSERT_OK_AND_ASSIGN(std::string bytes, gutil::ReadFile(pipeline_path));
  p4::v1::ForwardingPipelineConfig fpc;
  ASSERT_TRUE(fpc.ParseFromString(bytes)) << "Failed to parse pipeline";
  LOG(INFO) << "Pipeline: " << fpc.p4info().tables_size() << " tables, "
            << fpc.p4_device_config().size() << " bytes device config";

  // Load pipeline onto both servers via P4Runtime sessions.
  {
    ASSERT_OK_AND_ASSIGN(
        auto session,
        pdpi::P4RuntimeSession::Create(
            sut_server.Address(), grpc::InsecureChannelCredentials(),
            sut_server.DeviceId()));
    ASSERT_OK(pdpi::SetMetadataAndSetForwardingPipelineConfig(
        session.get(),
        p4::v1::SetForwardingPipelineConfigRequest::VERIFY_AND_COMMIT, fpc));
    LOG(INFO) << "Pipeline loaded on SUT";
  }
  {
    ASSERT_OK_AND_ASSIGN(
        auto session,
        pdpi::P4RuntimeSession::Create(
            control_server.Address(), grpc::InsecureChannelCredentials(),
            control_server.DeviceId()));
    ASSERT_OK(pdpi::SetMetadataAndSetForwardingPipelineConfig(
        session.get(),
        p4::v1::SetForwardingPipelineConfigRequest::VERIFY_AND_COMMIT, fpc));
    LOG(INFO) << "Pipeline loaded on control switch";
  }

  const auto& ir_p4info =
      sai::GetIrP4Info(sai::Instantiation::kMiddleblock);

  // Start fake gNMI servers (one per switch) for DVaaS port discovery.
  FakeGnmiServer sut_gnmi;
  FakeGnmiServer control_gnmi;
  LOG(INFO) << "Fake gNMI servers: SUT=" << sut_gnmi.address
            << " control=" << control_gnmi.address;

  // Create the DataplaneValidator with our 4ward backend.
  auto backend =
      std::make_unique<FourwardDataplaneValidationBackend>(sut_server.Address());
  dvaas::DataplaneValidator validator(std::move(backend));

  // Create a MirrorTestbed backed by two 4ward instances + fake gNMI.
  FourwardMirrorTestbed testbed(
      sut_server.Address(), sut_server.DeviceId(), sut_gnmi.address,
      control_server.Address(), control_server.DeviceId(),
      control_gnmi.address);

  // Install forwarding and punt entries on the SUT.
  {
    ASSERT_OK_AND_ASSIGN(
        auto sut_session,
        pdpi::P4RuntimeSession::Create(testbed.Sut()));
    ASSERT_OK(sai::EntryBuilder()
                  .AddEntriesForwardingIpPacketsToGivenPort(
                      "2", sai::IpVersion::kIpv4)
                  .AddEntryPuntingAllPackets(sai::PuntAction::kCopy)
                  .InstallDedupedEntities(ir_p4info, *sut_session));
    LOG(INFO) << "Installed forwarding + punt entries on SUT";
  }

  // Start the PacketBridge (emulates physical links between the two instances).
  PacketBridge bridge(sut_server.Address(), control_server.Address());
  ASSERT_THAT(bridge.Start(), IsOk());

  // Configure DVaaS params.
  dvaas::DataplaneValidationParams params;

  // Identity port map: SUT port X = control switch port X.
  params.mirror_testbed_port_map_override =
      dvaas::MirrorTestbedP4rtPortIdMap::CreateIdentityMap();

  // Disable failure analysis (unimplemented backend methods).
  params.failure_enhancement_options.max_failures_to_attempt_to_replicate = 0;
  params.failure_enhancement_options.collect_packet_trace = false;
  params.failure_enhancement_options.max_number_of_failures_to_minimize = 0;
  params.reset_and_collect_counters = false;

  // Provide SAI P4 middleblock P4Info.
  dvaas::P4Specification spec;
  *spec.p4_symbolic_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  *spec.bmv2_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  params.specification_override = std::move(spec);

  // Run upstream DVaaS validation.
  LOG(INFO) << "Running upstream DVaaS validation against 4ward...";
  auto result = validator.ValidateDataplane(testbed, params);

  bridge.Stop();
  LOG(INFO) << "PacketBridge forwarded " << bridge.PacketsForwarded()
            << " packets";

  ASSERT_THAT(result.status(), IsOk())
      << "DVaaS validation failed: " << result.status().message();
  result->LogStatistics();
  EXPECT_THAT(result->HasSuccessRateOfAtLeast(1.0), IsOk())
      << "DVaaS validation found failures.";
}

}  // namespace
}  // namespace fourward

// Custom main: parse absl flags before running gtest.
int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);
  return RUN_ALL_TESTS();
}
