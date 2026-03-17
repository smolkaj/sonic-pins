#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dvaas/dataplane_validation.h"
#include "fourward/fourward_backend.h"
#include "fourward/fourward_switch.h"
#include "gtest/gtest.h"
#include "gutil/gutil/status_matchers.h"
#include "gutil/gutil/version.h"
#include "p4_infra/p4_pdpi/ir.h"
#include "p4_infra/p4_pdpi/p4_runtime_session.h"
#include "p4_infra/p4_pdpi/p4_runtime_session_extras.h"
#include "p4_infra/p4_pdpi/pd.h"
#include "sai_p4/instantiations/google/instantiations.h"
#include "sai_p4/instantiations/google/sai_p4info.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

namespace fourward {
namespace {

using ::gutil::IsOk;

TEST(FourwardDvaasTest, ValidateDataplaneSteps) {
  const auto& ir_p4info = sai::GetIrP4Info(sai::Instantiation::kMiddleblock);

  FourwardMirrorTestbed testbed(
      "localhost:9559", 1, "localhost:9560", 2);

  // Step 1: Install entries on SUT
  LOG(INFO) << "=== Step 1: Install entries on SUT ===";
  {
    ASSERT_OK_AND_ASSIGN(auto sut_session,
                         pdpi::P4RuntimeSession::Create(testbed.Sut()));
    ASSERT_OK(sai::EntryBuilder()
                  .AddEntriesForwardingIpPacketsToGivenPort("2", sai::IpVersion::kIpv4)
                  .AddEntryPuntingAllPackets(sai::PuntAction::kCopy)
                  .InstallDedupedEntities(ir_p4info, *sut_session));
    LOG(INFO) << "  OK";
  }

  // Step 2: Create sessions (exactly as ValidateDataplane does)
  LOG(INFO) << "=== Step 2: Create sessions ===";
  ASSERT_OK_AND_ASSIGN(auto sut_p4rt,
                       pdpi::P4RuntimeSession::Create(testbed.Sut()));
  ASSERT_OK_AND_ASSIGN(auto sut_gnmi, testbed.Sut().CreateGnmiStub());
  ASSERT_OK_AND_ASSIGN(auto ctrl_p4rt,
                       pdpi::P4RuntimeSession::Create(testbed.ControlSwitch()));
  ASSERT_OK_AND_ASSIGN(auto ctrl_gnmi, testbed.ControlSwitch().CreateGnmiStub());
  LOG(INFO) << "  OK: All 4 connections created";

  // Step 3: ReadPiEntitiesSorted (what ValidateDataplaneUsingExistingSwitchApis does first)
  LOG(INFO) << "=== Step 3: ReadPiEntitiesSorted ===";
  ASSERT_OK_AND_ASSIGN(auto original_entities,
                       pdpi::ReadPiEntitiesSorted(*sut_p4rt));
  LOG(INFO) << "  OK: " << original_entities.size() << " entities";

  // Step 4: GetPkgInfoVersion (inside IncreasePerEntryRateLimitsToAvoidBogusDrops)
  LOG(INFO) << "=== Step 4: GetPkgInfoVersion ===";
  ASSERT_OK_AND_ASSIGN(auto version, pdpi::GetPkgInfoVersion(sut_p4rt.get()));
  LOG(INFO) << "  OK: version = " << "(parsed ok)";

  // Step 5: GetIrP4Info from control switch
  LOG(INFO) << "=== Step 5: GetIrP4Info (control) ===";
  ASSERT_OK_AND_ASSIGN(auto ir_info, pdpi::GetIrP4Info(*ctrl_p4rt));
  LOG(INFO) << "  OK";

  // Step 6: ClearEntities on control switch
  LOG(INFO) << "=== Step 6: ClearEntities (control) ===";
  ASSERT_OK(pdpi::ClearEntities(*ctrl_p4rt));
  LOG(INFO) << "  OK";

  // Step 7: Install punt entries on control switch
  LOG(INFO) << "=== Step 7: Install punt entries ===";
  {
    auto backend = std::make_unique<FourwardBackend>("localhost:9559");
    ASSERT_OK_AND_ASSIGN(auto punt, backend->GetEntitiesToPuntAllPackets(ir_info));
    LOG(INFO) << "  Got " << punt.entities_size() << " punt entries";
    ASSERT_OK(pdpi::InstallIrEntities(*ctrl_p4rt, punt));
    LOG(INFO) << "  OK: Installed";
  }

  // Step 8: GenerateTestVectors (the expensive part)
  LOG(INFO) << "=== Step 8: Create validator and generate test vectors ===";
  auto backend = std::make_unique<FourwardBackend>("localhost:9559");

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

  LOG(INFO) << "  About to call ValidateDataplane...";
  dvaas::DataplaneValidator validator(std::move(backend));
  auto result = validator.ValidateDataplane(testbed, params);
  if (result.ok()) {
    LOG(INFO) << "  OK!";
    result->LogStatistics();
  } else {
    LOG(ERROR) << "  FAILED: " << result.status();
  }
  ASSERT_THAT(result.status(), IsOk());
}

}  // namespace
}  // namespace fourward
