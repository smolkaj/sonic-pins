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

// DataplaneValidationBackend implementation for 4ward.
//
// This backend is intentionally minimal: it only implements
// GetEntitiesToPuntAllPackets (required for control switch setup). All other
// methods return UNIMPLEMENTED — they are unreachable when using
// packet_test_vector_override with specification_override set and failure
// analysis disabled.

#ifndef PINS_FOURWARD_FOURWARD_BACKEND_H_
#define PINS_FOURWARD_FOURWARD_BACKEND_H_

#include "dvaas/dataplane_validation.h"

namespace fourward {

// Minimal DataplaneValidationBackend for 4ward integration.
//
// Only GetEntitiesToPuntAllPackets has a real implementation — it returns
// SAI P4 ACL entries that punt all traffic. All other methods return
// UNIMPLEMENTED since they're unreachable when using packet_test_vector_override
// with failure analysis disabled.
class FourwardBackend : public dvaas::DataplaneValidationBackend {
 public:
  absl::StatusOr<dvaas::PacketSynthesisResult> SynthesizePackets(
      const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
      const p4::v1::ForwardingPipelineConfig& p4_symbolic_config,
      absl::Span<const pins_test::P4rtPortId> ports,
      const dvaas::OutputWriterFunctionType& write_stats,
      const std::optional<p4_symbolic::packet_synthesizer::CoverageGoals>&
          coverage_goals_override,
      std::optional<absl::Duration> time_limit) const override {
    return absl::UnimplementedError(
        "4ward backend does not support packet synthesis. "
        "Use packet_test_vector_override instead.");
  }

  absl::StatusOr<dvaas::PacketTestVectorById> GeneratePacketTestVectors(
      const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
      const p4::v1::ForwardingPipelineConfig& bmv2_config,
      absl::Span<const pins_test::P4rtPortId> ports,
      std::vector<p4_symbolic::packet_synthesizer::SynthesizedPacket>&
          synthesized_packets,
      const pins_test::P4rtPortId& default_ingress_port,
      bool check_prediction_conformity) const override {
    return absl::UnimplementedError(
        "4ward backend does not support GeneratePacketTestVectors. "
        "Use packet_test_vector_override instead.");
  }

  // Returns SAI P4 ACL entries that punt all received packets.
  // This is required to set up the control switch in a mirror testbed.
  absl::StatusOr<pdpi::IrEntities> GetEntitiesToPuntAllPackets(
      const pdpi::IrP4Info& switch_p4info) const override;

  absl::StatusOr<dvaas::P4Specification> InferP4Specification(
      dvaas::SwitchApi& sut) const override {
    return absl::UnimplementedError(
        "4ward backend does not support InferP4Specification. "
        "Use specification_override instead.");
  }

  absl::Status AugmentPacketTestVectorsWithPacketTraces(
      std::vector<dvaas::PacketTestVector>& packet_test_vectors,
      const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
      const p4::v1::ForwardingPipelineConfig& bmv2_compatible_config,
      bool use_compact_traces) const override {
    return absl::UnimplementedError(
        "4ward backend does not support packet trace augmentation.");
  }

  absl::StatusOr<pdpi::IrEntities> CreateV1ModelAuxiliaryEntities(
      const pdpi::IrEntities& ir_entities, const pdpi::IrP4Info& ir_p4info,
      gnmi::gNMI::StubInterface& gnmi_stub) const override {
    // Return empty entities — 4ward doesn't need v1model auxiliary tables.
    return pdpi::IrEntities();
  }
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_BACKEND_H_
