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

#include "fourward/fourward_dataplane_validation_backend.h"

#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "dvaas/test_vector.h"
#include "dvaas/test_vector.pb.h"
#include "gutil/gutil/proto.h"
#include "gutil/gutil/test_artifact_writer.h"
#include "fourward/dataplane.grpc.pb.h"
#include "fourward/dataplane.pb.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/gutil/status.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"
#include "p4_infra/p4_pdpi/ir.pb.h"
#include "p4_infra/packetlib/packetlib.h"
#include "p4_infra/packetlib/packetlib.pb.h"
#include "p4_symbolic/packet_synthesizer/packet_synthesizer.pb.h"

namespace fourward {

FourwardDataplaneValidationBackend::FourwardDataplaneValidationBackend(
    std::string sut_address)
    : channel_(grpc::CreateChannel(std::move(sut_address),
                                   grpc::InsecureChannelCredentials())),
      stub_(dataplane::Dataplane::NewStub(channel_)) {}

absl::StatusOr<dvaas::PacketSynthesisResult>
FourwardDataplaneValidationBackend::SynthesizePackets(
    const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
    const p4::v1::ForwardingPipelineConfig& p4_symbolic_config,
    absl::Span<const pins_test::P4rtPortId> ports,
    const dvaas::OutputWriterFunctionType& write_stats,
    const std::optional<p4_symbolic::packet_synthesizer::CoverageGoals>&
        coverage_goals_override,
    std::optional<absl::Duration> time_limit) const {
  if (ports.empty()) {
    return absl::InvalidArgumentError("No ports available for test packets");
  }

  dvaas::PacketSynthesisResult result;

  // Hardcoded test packet: a simple IPv4 UDP packet.
  // The payload will be replaced with a test tag by GeneratePacketTestVectors.
  packetlib::Packet packet_proto;
  auto* eth = packet_proto.add_headers()->mutable_ethernet_header();
  // Unicast MAC required: SAI P4 gates L3 routing on IS_UNICAST_MAC(dst_addr).
  eth->set_ethernet_destination("00:01:02:03:04:05");
  eth->set_ethernet_source("00:00:00:00:00:01");
  eth->set_ethertype("0x0800");

  auto* ipv4 = packet_proto.add_headers()->mutable_ipv4_header();
  ipv4->set_version("0x4");
  ipv4->set_ihl("0x5");
  ipv4->set_dscp("0x00");
  ipv4->set_ecn("0x0");
  ipv4->set_identification("0x0001");
  ipv4->set_flags("0x0");
  ipv4->set_fragment_offset("0x0000");
  ipv4->set_ttl("0x40");
  ipv4->set_protocol("0x11");  // UDP
  ipv4->set_ipv4_source("192.168.0.1");
  ipv4->set_ipv4_destination("192.168.0.2");

  auto* udp = packet_proto.add_headers()->mutable_udp_header();
  udp->set_source_port("0x0000");
  udp->set_destination_port("0x0000");

  packet_proto.set_payload("placeholder");

  ASSIGN_OR_RETURN(std::string raw_packet,
                   packetlib::SerializePacket(packet_proto));

  p4_symbolic::packet_synthesizer::SynthesizedPacket synthesized;
  synthesized.set_packet(raw_packet);
  synthesized.set_ingress_port(ports[0].GetP4rtEncoding());
  synthesized.set_drop_expected(false);
  synthesized.set_punt_expected(false);

  result.synthesized_packets.push_back(std::move(synthesized));

  LOG(INFO) << "4ward backend: synthesized "
            << result.synthesized_packets.size()
            << " hardcoded test packet(s)";
  return result;
}

absl::StatusOr<dvaas::PacketTestVectorById>
FourwardDataplaneValidationBackend::GeneratePacketTestVectors(
    const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
    const p4::v1::ForwardingPipelineConfig& bmv2_config,
    absl::Span<const pins_test::P4rtPortId> ports,
    std::vector<p4_symbolic::packet_synthesizer::SynthesizedPacket>&
        synthesized_packets,
    const pins_test::P4rtPortId& default_ingress_port,
    bool check_prediction_conformity) const {
  dvaas::PacketTestVectorById test_vectors;

  for (size_t i = 0; i < synthesized_packets.size(); ++i) {
    auto& synthesized = synthesized_packets[i];
    int test_id = static_cast<int>(i) + 1;

    std::string ingress_port = synthesized.ingress_port();
    if (ingress_port.empty()) {
      ingress_port = default_ingress_port.GetP4rtEncoding();
    }

    packetlib::Packet parsed = packetlib::ParsePacket(synthesized.packet());
    parsed.set_payload(dvaas::MakeTestPacketTagFromUniqueId(
        test_id, "4ward hardcoded test"));
    RETURN_IF_ERROR(packetlib::UpdateAllComputedFields(parsed).status());
    ASSIGN_OR_RETURN(std::string tagged_packet,
                     packetlib::SerializePacket(parsed));

    synthesized.set_packet(tagged_packet);

    dvaas::PacketTestVector test_vector;
    auto* input = test_vector.mutable_input();
    input->set_type(dvaas::SwitchInput::DATAPLANE);
    auto* input_packet = input->mutable_packet();
    input_packet->set_port(ingress_port);
    input_packet->set_hex(absl::BytesToHexString(tagged_packet));
    *input_packet->mutable_parsed() = parsed;

    dataplane::InjectPacketRequest inject_request;
    // Use P4RT port encoding for consistency with table entries (which are
    // installed via P4Runtime and go through 4ward's port translator).
    inject_request.set_p4rt_ingress_port(ingress_port);
    inject_request.set_payload(tagged_packet);

    LOG(INFO) << "InjectPacket request: p4rt_ingress_port=\""
              << absl::BytesToHexString(inject_request.p4rt_ingress_port())
              << "\" (" << inject_request.p4rt_ingress_port().size()
              << " bytes), payload=" << tagged_packet.size() << " bytes";

    grpc::ClientContext ctx;
    dataplane::InjectPacketResponse inject_response;
    auto grpc_status =
        stub_->InjectPacket(&ctx, inject_request, &inject_response);
    LOG(INFO) << "InjectPacket gRPC status: code=" << grpc_status.error_code()
              << " message=\"" << grpc_status.error_message() << "\""
              << " outputs=" << inject_response.output_packets().size()
              << " trace_bytes=" << inject_response.trace().size();
    // Save trace as a Bazel test artifact for post-mortem analysis.
    if (auto status = artifact_writer_.StoreTestArtifact(
            absl::StrCat("fourward_trace_", test_id, ".bin"),
            inject_response.trace());
        !status.ok()) {
      LOG(WARNING) << "Failed to save trace artifact: " << status;
    }
    RETURN_IF_ERROR(gutil::GrpcStatusToAbslStatus(grpc_status))
        << "injecting packet into 4ward for prediction";

    auto* output = test_vector.add_acceptable_outputs();
    for (const auto& out_pkt : inject_response.output_packets()) {
      auto* predicted = output->add_packets();
      // Use P4RT port encoding when available (consistent with table entries).
      // Fall back to dataplane port — 4ward's InjectPacket response doesn't
      // always populate p4rt_egress_port even with a loaded pipeline.
      // TODO(4ward): Always populate p4rt_egress_port in InjectPacketResponse.
      if (!out_pkt.p4rt_egress_port().empty()) {
        predicted->set_port(out_pkt.p4rt_egress_port());
      } else {
        predicted->set_port(absl::StrCat(out_pkt.dataplane_egress_port()));
      }
      const std::string& out_payload = out_pkt.payload();
      predicted->set_hex(absl::BytesToHexString(out_payload));
      *predicted->mutable_parsed() = packetlib::ParsePacket(out_payload);
    }

    // Guard against silent DROP predictions. If the synthesized packet was
    // not expected to be dropped but InjectPacket returned zero outputs,
    // something is wrong (pipeline not loaded, entries missing, etc.).
    if (inject_response.output_packets().empty() &&
        !synthesized.drop_expected()) {
      return absl::InternalError(absl::StrCat(
          "Test #", test_id,
          ": InjectPacket returned zero output packets, but the synthesized "
          "packet was not expected to be dropped. This likely indicates a "
          "missing pipeline or table entries."));
    }

    LOG(INFO) << "Test #" << test_id << ": predicted "
              << inject_response.output_packets().size() << " output(s)";
    VLOG(1) << "Test vector #" << test_id << ":\n"
             << gutil::PrintTextProto(test_vector);

    test_vectors[test_id] = std::move(test_vector);
  }

  LOG(INFO) << "4ward backend: generated " << test_vectors.size()
            << " test vector(s) with output predictions";
  return test_vectors;
}

absl::StatusOr<pdpi::IrEntities>
FourwardDataplaneValidationBackend::GetEntitiesToPuntAllPackets(
    const pdpi::IrP4Info& switch_p4info) const {
  return sai::EntryBuilder()
      .AddEntryPuntingAllPackets(sai::PuntAction::kTrap)
      .GetDedupedIrEntities(switch_p4info);
}

}  // namespace fourward
