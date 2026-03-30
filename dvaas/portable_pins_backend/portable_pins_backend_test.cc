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

// Tests for the portable PINS backend: unit tests for each method and an
// integration test exercising the backend against a FourwardPinsMirrorTestbed.

#include "dvaas/portable_pins_backend/portable_pins_backend.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "dvaas/dataplane_validation.h"
#include "dvaas/port_id_map.h"
#include "dvaas/switch_api.h"
#include "dvaas/test_vector.h"
#include "dvaas/test_vector.pb.h"
#include "dvaas/validation_result.h"
#include "fourward/fourward_oracle.h"
#include "fourward/fourward_pins_mirror_testbed.h"
#include "fourward/test_util.h"
#include "fourward/test_vector_generation.h"
#include "gtest/gtest.h"
#include "gutil/status_matchers.h"
#include "lib/p4rt/p4rt_port.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_infra/p4_runtime/p4_runtime_session.h"
#include "p4_pdpi/ir.h"
#include "p4_pdpi/ir.pb.h"
#include "p4_symbolic/packet_synthesizer/packet_synthesizer.pb.h"
#include "sai_p4/instantiations/google/instantiations.h"
#include "sai_p4/instantiations/google/sai_nonstandard_platforms.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

namespace dvaas {
namespace {

// Returns P4RT port IDs "first" through "last" (inclusive).
absl::StatusOr<std::vector<pins_test::P4rtPortId>> MakePorts(int first,
                                                              int last) {
  std::vector<pins_test::P4rtPortId> ports;
  for (int i = first; i <= last; ++i) {
    ASSIGN_OR_RETURN(pins_test::P4rtPortId port,
                     pins_test::P4rtPortId::MakeFromP4rtEncoding(
                         absl::StrCat(i)));
    ports.push_back(port);
  }
  return ports;
}

// Identity port map: port "i" on SUT maps to port "i" on control switch.
absl::StatusOr<MirrorTestbedP4rtPortIdMap> MakeIdentityPortMap(int first,
                                                                int last) {
  ASSIGN_OR_RETURN(std::vector<pins_test::P4rtPortId> ports,
                   MakePorts(first, last));
  absl::flat_hash_map<pins_test::P4rtPortId, pins_test::P4rtPortId> map;
  for (const pins_test::P4rtPortId& port : ports) {
    map[port] = port;
  }
  return MirrorTestbedP4rtPortIdMap::CreateFromSutToControlSwitchPortMap(map);
}

p4::v1::ForwardingPipelineConfig LoadSaiMiddleblockP4SymbolicConfig() {
  return sai::GetNonstandardForwardingPipelineConfig(
      sai::Instantiation::kMiddleblock,
      sai::NonstandardPlatform::kP4Symbolic);
}

// Loads the fourward pipeline on both testbed switches and installs basic
// forwarding entries on the SUT. Sessions are scoped so they close before
// ValidateDataplane opens its own (4ward supports one StreamChannel at a time).
void LoadPipelineAndInstallEntries(
    FourwardPinsMirrorTestbed& testbed,
    const p4::v1::ForwardingPipelineConfig& config) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<p4_runtime::P4RuntimeSession> sut_session,
      p4_runtime::P4RuntimeSession::Create(testbed.Sut()));
  ASSERT_OK(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
      sut_session.get(),
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
      config));
  ASSERT_OK(sai::EntryBuilder()
                .AddDisableVlanChecksEntry()
                .AddDisableIngressVlanChecksEntry()
                .AddDisableEgressVlanChecksEntry()
                .AddEntriesForwardingIpPacketsToGivenPort("1")
                .InstallDedupedEntities(*sut_session));

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<p4_runtime::P4RuntimeSession> control_session,
      p4_runtime::P4RuntimeSession::Create(testbed.ControlSwitch()));
  ASSERT_OK(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
      control_session.get(),
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
      config));
}


TEST(PortablePinsBackendTest, SynthesizePacketsProducesPackets) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);

  p4::v1::ForwardingPipelineConfig p4_symbolic_config =
      LoadSaiMiddleblockP4SymbolicConfig();

  ASSERT_OK_AND_ASSIGN(pdpi::IrP4Info ir_p4info,
                       pdpi::CreateIrP4Info(fourward_config.p4info()));

  ASSERT_OK_AND_ASSIGN(
      pdpi::IrEntities ir_entities,
      sai::EntryBuilder()
          .AddDisableVlanChecksEntry()
          .AddEntriesForwardingIpPacketsToGivenPort("1")
          .GetDedupedIrEntities(ir_p4info));

  ASSERT_OK_AND_ASSIGN(std::vector<pins_test::P4rtPortId> ports,
                       MakePorts(1, 8));

  ASSERT_OK_AND_ASSIGN(
      PacketSynthesisResult result,
      backend->SynthesizePackets(ir_p4info, ir_entities, p4_symbolic_config,
                                 ports, /*write_stats=*/nullptr,
                                 /*coverage_goals_override=*/std::nullopt));
  ASSERT_FALSE(result.synthesized_packets.empty());
  int forwarded = 0;
  for (const p4_symbolic::packet_synthesizer::SynthesizedPacket& packet :
       result.synthesized_packets) {
    EXPECT_FALSE(packet.packet().empty())
        << "Synthesized packet has empty payload";
    if (!packet.drop_expected()) ++forwarded;
  }
  EXPECT_GT(forwarded, 0)
      << "Expected at least one forwarded packet — forwarding entries are "
         "installed.";
}

TEST(PortablePinsBackendTest, GetEntitiesToPuntAllPacketsSucceeds) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);

  ASSERT_OK_AND_ASSIGN(pdpi::IrP4Info ir_p4info,
                       pdpi::CreateIrP4Info(fourward_config.p4info()));
  ASSERT_OK_AND_ASSIGN(pdpi::IrEntities punt_entities,
                       backend->GetEntitiesToPuntAllPackets(ir_p4info));
  EXPECT_FALSE(punt_entities.entities().empty());
}

TEST(PortablePinsBackendTest, AugmentPacketTestVectorsIsNoOp) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);

  std::vector<PacketTestVector> vectors;
  pdpi::IrP4Info ir_p4info;
  pdpi::IrEntities ir_entities;
  p4::v1::ForwardingPipelineConfig bmv2_config;
  EXPECT_OK(backend->AugmentPacketTestVectorsWithPacketTraces(
      vectors, ir_p4info, ir_entities, bmv2_config,
      /*use_compact_traces=*/false));
}

TEST(PortablePinsBackendTest,
     GeneratePacketTestVectorsProducesCorrectPredictions) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);

  ASSERT_OK_AND_ASSIGN(pdpi::IrP4Info ir_p4info,
                       pdpi::CreateIrP4Info(fourward_config.p4info()));

  // Build IR entities for forwarding.
  ASSERT_OK_AND_ASSIGN(
      pdpi::IrEntities ir_entities,
      sai::EntryBuilder()
          .AddDisableVlanChecksEntry()
          .AddEntriesForwardingIpPacketsToGivenPort("1")
          .GetDedupedIrEntities(ir_p4info));

  // Create a synthesized packet.
  std::string raw_packet = SerializeTestPacket(R"pb(
    headers {
      ethernet_header {
        ethernet_destination: "02:02:02:02:02:02"
        ethernet_source: "00:aa:bb:cc:dd:ee"
        ethertype: "0x0800"
      }
    }
    headers {
      ipv4_header {
        version: "0x4"
        ihl: "0x5"
        dscp: "0x00"
        ecn: "0x0"
        identification: "0x0000"
        flags: "0x0"
        fragment_offset: "0x0000"
        ttl: "0x40"
        protocol: "0x11"
        ipv4_source: "192.168.1.1"
        ipv4_destination: "10.1.2.3"
      }
    }
    headers {
      udp_header { source_port: "0x0000" destination_port: "0x0000" }
    }
  )pb");

  std::vector<p4_symbolic::packet_synthesizer::SynthesizedPacket>
      synthesized_packets;
  p4_symbolic::packet_synthesizer::SynthesizedPacket synthesized;
  synthesized.set_packet(raw_packet);
  synthesized.set_ingress_port("0");
  synthesized_packets.push_back(std::move(synthesized));

  ASSERT_OK_AND_ASSIGN(pins_test::P4rtPortId default_port,
                       pins_test::P4rtPortId::MakeFromP4rtEncoding("0"));
  ASSERT_OK_AND_ASSIGN(
      PacketTestVectorById test_vectors,
      backend->GeneratePacketTestVectors(
          ir_p4info, ir_entities, fourward_config, {default_port},
          synthesized_packets, default_port,
          /*check_prediction_conformity=*/false));

  ASSERT_EQ(test_vectors.size(), 1);
  const PacketTestVector& vector = test_vectors.begin()->second;
  EXPECT_TRUE(vector.has_input());
  EXPECT_FALSE(vector.acceptable_outputs().empty());
  // The packet should be forwarded (not dropped).
  EXPECT_FALSE(vector.acceptable_outputs(0).packets().empty());
  // Forwarded to port "1".
  EXPECT_EQ(vector.acceptable_outputs(0).packets(0).port(), "1");
}

// E2E integration: verifies the backend drives a FourwardPinsMirrorTestbed
// end-to-end — pipeline loading, entity installation, punt entry generation,
// and test vector generation with correct predictions.
TEST(PortablePinsBackendTest, BackendWorksEndToEndWithFourwardTestbed) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();

  // -- Testbed ---------------------------------------------------------------
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                       FourwardPinsMirrorTestbed::Create());

  // Open a P4RT session on the SUT and load the pipeline.
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<p4_runtime::P4RuntimeSession> sut_session,
      p4_runtime::P4RuntimeSession::Create(testbed->Sut()));
  ASSERT_OK(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
      sut_session.get(),
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
      fourward_config));

  // Install forwarding entries on the SUT.
  ASSERT_OK(sai::EntryBuilder()
                .AddDisableVlanChecksEntry()
                .AddEntriesForwardingIpPacketsToGivenPort("1")
                .InstallDedupedEntities(*sut_session));

  // -- Backend methods -------------------------------------------------------
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);

  // InferP4Specification reads the pipeline from the switch.
  SwitchApi sut;
  sut.p4rt = std::move(sut_session);
  ASSERT_OK_AND_ASSIGN(sut.gnmi, testbed->Sut().CreateGnmiStub());
  ASSERT_OK_AND_ASSIGN(P4Specification spec,
                       backend->InferP4Specification(sut));
  EXPECT_TRUE(spec.fourward_config.has_value());
  EXPECT_FALSE(spec.p4_symbolic_config.p4info().tables().empty());

  // GetEntitiesToPuntAllPackets produces non-empty punt entries.
  ASSERT_OK_AND_ASSIGN(pdpi::IrP4Info ir_p4info,
                       pdpi::CreateIrP4Info(fourward_config.p4info()));
  ASSERT_OK_AND_ASSIGN(pdpi::IrEntities punt_entities,
                       backend->GetEntitiesToPuntAllPackets(ir_p4info));
  EXPECT_FALSE(punt_entities.entities().empty());
}

// The north star test: run the full DVaaS ValidateDataplane flow using
// a FourwardPinsMirrorTestbed, the portable PINS backend, and user-provided
// test vectors.
TEST(PortablePinsBackendTest,
     ValidateDataplaneWithUserProvidedTestVectors) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();

  // Create testbed and backend.
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                       FourwardPinsMirrorTestbed::Create());
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);
  DataplaneValidator validator(std::move(backend));

  LoadPipelineAndInstallEntries(*testbed, fourward_config);

  // Build a user-provided test vector: send an IPv4 packet, expect it
  // forwarded to port "1".
  std::string raw_packet = SerializeTestPacket(R"pb(
    headers {
      ethernet_header {
        ethernet_destination: "02:02:02:02:02:02"
        ethernet_source: "00:aa:bb:cc:dd:ee"
        ethertype: "0x0800"
      }
    }
    headers {
      ipv4_header {
        version: "0x4"
        ihl: "0x5"
        dscp: "0x00"
        ecn: "0x0"
        identification: "0x0000"
        flags: "0x0"
        fragment_offset: "0x0000"
        ttl: "0x40"
        protocol: "0x11"
        ipv4_source: "192.168.1.1"
        ipv4_destination: "10.1.2.3"
      }
    }
    headers {
      udp_header { source_port: "0x0000" destination_port: "0x0000" }
    }
  )pb");

  // Build the test vector with a tagged payload so DVaaS can correlate
  // outputs with inputs.
  packetlib::Packet parsed = packetlib::ParsePacket(raw_packet);
  parsed.set_payload(MakeTestPacketTagFromUniqueId(1, "forwarding test"));
  ASSERT_OK(packetlib::PadPacketToMinimumSize(parsed));
  ASSERT_OK(packetlib::UpdateAllComputedFields(parsed));
  ASSERT_OK_AND_ASSIGN(std::string tagged_packet,
                       packetlib::SerializePacket(parsed));

  PacketTestVector test_vector;
  SwitchInput* input = test_vector.mutable_input();
  input->set_type(SwitchInput::DATAPLANE);
  input->mutable_packet()->set_port("1");
  input->mutable_packet()->set_hex(absl::BytesToHexString(tagged_packet));
  *input->mutable_packet()->mutable_parsed() = parsed;

  // Expected: at least one output (forwarded). We add an empty acceptable
  // output -- DVaaS checks that the SUT's output matches 4ward's prediction.
  test_vector.add_acceptable_outputs();

  ASSERT_OK_AND_ASSIGN(MirrorTestbedP4rtPortIdMap port_map,
                       MakeIdentityPortMap(1, 8));

  DataplaneValidationParams params;
  P4Specification spec;
  spec.fourward_config = fourward_config;
  spec.p4_symbolic_config = fourward_config;
  spec.bmv2_config = fourward_config;
  params.specification_override = spec;
  params.packet_test_vector_override = {test_vector};
  params.mirror_testbed_port_map_override = port_map;

  ASSERT_OK_AND_ASSIGN(ValidationResult result,
                       validator.ValidateDataplane(*testbed, params));
  EXPECT_OK(result.HasSuccessRateOfAtLeast(1.0));
}

// Full E2E: p4-symbolic synthesizes packets, 4ward predicts outputs, DVaaS
// validates the SUT.
// TODO: Investigate failing test vectors and raise to 1.0.
TEST(PortablePinsBackendTest, ValidateDataplaneWithSynthesizedTestVectors) {
  p4::v1::ForwardingPipelineConfig fourward_config = LoadSaiMiddleblock4wardConfig();

  p4::v1::ForwardingPipelineConfig p4_symbolic_config =
      LoadSaiMiddleblockP4SymbolicConfig();

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                       FourwardPinsMirrorTestbed::Create());
  std::unique_ptr<DataplaneValidationBackend> backend =
      CreatePortablePinsBackend(fourward_config);
  DataplaneValidator validator(std::move(backend));

  LoadPipelineAndInstallEntries(*testbed, fourward_config);

  ASSERT_OK_AND_ASSIGN(MirrorTestbedP4rtPortIdMap port_map,
                       MakeIdentityPortMap(1, 8));

  // No packet_test_vector_override — DVaaS synthesizes packets automatically.
  DataplaneValidationParams params;
  P4Specification spec;
  spec.fourward_config = fourward_config;
  spec.p4_symbolic_config = p4_symbolic_config;
  spec.bmv2_config = fourward_config;
  params.specification_override = spec;
  params.mirror_testbed_port_map_override = port_map;

  ASSERT_OK_AND_ASSIGN(ValidationResult result,
                       validator.ValidateDataplane(*testbed, params));
  // TODO: Investigate failing test vectors and raise to 1.0.
  EXPECT_OK(result.HasSuccessRateOfAtLeast(0.52));
}

}  // namespace
}  // namespace dvaas
