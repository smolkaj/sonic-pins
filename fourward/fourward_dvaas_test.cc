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
// instances, connected by a PacketBridge.

#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dvaas/dataplane_validation.h"
#include "fourward/fourward_backend.h"
#include "fourward/fourward_switch.h"
#include "fourward/packet_bridge.h"
#include "gtest/gtest.h"
#include "gutil/gutil/status_matchers.h"
#include "p4_infra/p4_pdpi/p4_runtime_session.h"
#include "p4_infra/p4_pdpi/p4_runtime_session_extras.h"
#include "p4_infra/p4_pdpi/pd.h"
#include "sai_p4/instantiations/google/instantiations.h"
#include "sai_p4/instantiations/google/sai_p4info.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

ABSL_FLAG(std::string, sut_address, "localhost:9559",
          "Address of the 4ward SUT P4RuntimeServer.");
ABSL_FLAG(std::string, control_address, "localhost:9560",
          "Address of the 4ward control switch P4RuntimeServer.");
ABSL_FLAG(uint32_t, sut_device_id, 1, "P4Runtime device ID for the SUT.");
ABSL_FLAG(uint32_t, control_device_id, 2,
          "P4Runtime device ID for the control switch.");

namespace fourward {
namespace {

using ::gutil::IsOk;

TEST(FourwardDvaasTest, UpstreamDvaasValidation) {
  const auto& ir_p4info =
      sai::GetIrP4Info(sai::Instantiation::kMiddleblock);

  // Create the DataplaneValidator with our 4ward backend.
  auto backend = std::make_unique<FourwardBackend>(
      absl::GetFlag(FLAGS_sut_address));
  dvaas::DataplaneValidator validator(std::move(backend));

  // Create a MirrorTestbed backed by two 4ward instances.
  FourwardMirrorTestbed testbed(
      absl::GetFlag(FLAGS_sut_address), absl::GetFlag(FLAGS_sut_device_id),
      absl::GetFlag(FLAGS_control_address),
      absl::GetFlag(FLAGS_control_device_id));

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

  // Smoke test: verify P4RT Read works before starting DVaaS.
  {
    ASSERT_OK_AND_ASSIGN(
        auto sut_session,
        pdpi::P4RuntimeSession::Create(testbed.Sut()));
    auto entities = pdpi::ReadPiEntities(sut_session.get());
    ASSERT_THAT(entities.status(), IsOk())
        << "P4RT Read smoke test failed: " << entities.status().message();
    LOG(INFO) << "Smoke test: Read " << entities->size()
              << " entities from SUT";
  }

  // Start the PacketBridge to emulate physical links between the two instances.
  PacketBridge bridge(absl::GetFlag(FLAGS_sut_address),
                      absl::GetFlag(FLAGS_control_address));
  ASSERT_THAT(bridge.Start(), IsOk());

  // Configure DVaaS params.
  dvaas::DataplaneValidationParams params;

  // Provide identity port map (SUT port X = control switch port X), skipping
  // the gNMI-based port mirroring that ValidateDataplane does by default.
  params.mirror_testbed_port_map_override =
      dvaas::MirrorTestbedP4rtPortIdMap::CreateIdentityMap();

  // Disable failure analysis to avoid calling unimplemented backend methods.
  params.failure_enhancement_options.max_failures_to_attempt_to_replicate = 0;
  params.failure_enhancement_options.collect_packet_trace = false;
  params.failure_enhancement_options.max_number_of_failures_to_minimize = 0;
  params.reset_and_collect_counters = false;

  // Populate the P4Specification with SAI P4 middleblock P4Info.
  dvaas::P4Specification spec;
  *spec.p4_symbolic_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  *spec.bmv2_config.mutable_p4info() =
      sai::GetP4Info(sai::Instantiation::kMiddleblock);
  params.specification_override = std::move(spec);

  // Run upstream DVaaS validation!
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
