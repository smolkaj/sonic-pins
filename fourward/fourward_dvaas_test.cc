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
// instances connected by a PacketBridge.

#include <memory>
#include <string>

#include "absl/log/log.h"
#include "dvaas/dataplane_validation.h"
#include "fourward/fourward_dataplane_validation_backend.h"
#include "fourward/fourward_mirror_testbed.h"
#include "fourward/runfiles.h"
#include "gtest/gtest.h"
#include "grpcpp/security/credentials.h"
#include "gutil/gutil/io.h"
#include "gutil/gutil/status_matchers.h"
#include "p4_infra/p4_pdpi/p4_runtime_session.h"
#include "sai_p4/instantiations/google/instantiations.h"
#include "sai_p4/instantiations/google/sai_p4info.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

namespace fourward {
namespace {

using ::gutil::IsOk;

constexpr char kPipeline[] = "fourward/prebuilt/sai_middleblock.binpb";

TEST(FourwardDvaasTest, UpstreamDvaasValidation) {
  // Start two 4ward instances connected by a PacketBridge.
  ASSERT_OK_AND_ASSIGN(auto testbed, FourwardMirrorTestbed::Start());
  LOG(INFO) << "Testbed started: SUT=" << testbed->Sut().ChassisName()
            << " control=" << testbed->ControlSwitch().ChassisName();

  // Load pipeline onto both switches.
  ASSERT_OK_AND_ASSIGN(std::string bytes, gutil::ReadFile(fourward::BazelRunfile(kPipeline)));
  p4::v1::ForwardingPipelineConfig fpc;
  ASSERT_TRUE(fpc.ParseFromString(bytes)) << "Failed to parse pipeline";
  for (auto* sw : {&testbed->Sut(), &testbed->ControlSwitch()}) {
    ASSERT_OK_AND_ASSIGN(
        auto session,
        pdpi::P4RuntimeSession::Create(*sw));
    ASSERT_OK(pdpi::SetMetadataAndSetForwardingPipelineConfig(
        session.get(),
        p4::v1::SetForwardingPipelineConfigRequest::VERIFY_AND_COMMIT, fpc));
  }
  LOG(INFO) << "Pipeline loaded on both switches";

  // Install forwarding and punt entries on the SUT.
  const auto& ir_p4info =
      sai::GetIrP4Info(sai::Instantiation::kMiddleblock);
  {
    ASSERT_OK_AND_ASSIGN(
        auto session, pdpi::P4RuntimeSession::Create(testbed->Sut()));
    ASSERT_OK(sai::EntryBuilder()
                  .AddEntriesForwardingIpPacketsToGivenPort(
                      "2", sai::IpVersion::kIpv4)
                  .AddEntryPuntingAllPackets(sai::PuntAction::kCopy)
                  .InstallDedupedEntities(ir_p4info, *session));
  }
  LOG(INFO) << "Installed forwarding + punt entries on SUT";

  // Run DVaaS validation.
  auto backend = std::make_unique<FourwardDataplaneValidationBackend>(
      testbed->SutFourwardAddress());
  dvaas::DataplaneValidator validator(std::move(backend));

  dvaas::DataplaneValidationParams params;
  params.mirror_testbed_port_map_override =
      dvaas::MirrorTestbedP4rtPortIdMap::CreateIdentityMap();
  params.failure_enhancement_options.max_failures_to_attempt_to_replicate = 0;
  params.failure_enhancement_options.collect_packet_trace = false;
  params.failure_enhancement_options.max_number_of_failures_to_minimize = 0;
  params.reset_and_collect_counters = false;

  dvaas::P4Specification spec;
  *spec.p4_symbolic_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  *spec.bmv2_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  params.specification_override = std::move(spec);

  LOG(INFO) << "Running DVaaS validation...";
  auto result = validator.ValidateDataplane(*testbed, params);

  testbed->Bridge().Stop();
  LOG(INFO) << "PacketBridge forwarded " << testbed->Bridge().PacketsForwarded()
            << " packets";

  ASSERT_THAT(result.status(), IsOk())
      << "DVaaS validation failed: " << result.status().message();
  result->LogStatistics();
  EXPECT_THAT(result->HasSuccessRateOfAtLeast(1.0), IsOk());
}

}  // namespace
}  // namespace fourward
