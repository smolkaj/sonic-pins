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

#include "fourward/fourward_backend.h"

#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "dvaas/test_vector.h"
#include "dvaas/test_vector.pb.h"
#include "fourward/dataplane.grpc.pb.h"
#include "fourward/dataplane.pb.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/gutil/proto.h"
#include "gutil/gutil/status.h"
#include "p4_infra/p4_pdpi/ir.pb.h"
#include "p4_infra/packetlib/packetlib.h"
#include "p4_infra/packetlib/packetlib.pb.h"
#include "p4_symbolic/packet_synthesizer/packet_synthesizer.pb.h"

namespace fourward {

FourwardBackend::FourwardBackend(std::string sut_address)
    : sut_address_(std::move(sut_address)) {}

absl::StatusOr<dvaas::PacketSynthesisResult> FourwardBackend::SynthesizePackets(
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
  eth->set_ethernet_destination("ff:ff:ff:ff:ff:ff");
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
  // checksum and total_length are computed fields — left unset.

  auto* udp = packet_proto.add_headers()->mutable_udp_header();
  udp->set_source_port("0x0000");
  udp->set_destination_port("0x0000");
  // length and checksum are computed fields.

  // Placeholder payload (will be replaced with test tag).
  packet_proto.set_payload("placeholder");

  ASSIGN_OR_RETURN(std::string raw_packet,
                   packetlib::SerializePacket(packet_proto));

  p4_symbolic::packet_synthesizer::SynthesizedPacket synthesized;
  synthesized.set_packet(raw_packet);
  synthesized.set_ingress_port(ports[0].GetP4rtEncoding());
  synthesized.set_drop_expected(false);
  synthesized.set_punt_expected(false);

  result.synthesized_packets.push_back(std::move(synthesized));

  LOG(INFO) << "4ward backend: synthesized " << result.synthesized_packets.size()
            << " hardcoded test packet(s)";
  return result;
}

absl::StatusOr<dvaas::PacketTestVectorById>
FourwardBackend::GeneratePacketTestVectors(
    const pdpi::IrP4Info& ir_p4info, const pdpi::IrEntities& ir_entities,
    const p4::v1::ForwardingPipelineConfig& bmv2_config,
    absl::Span<const pins_test::P4rtPortId> ports,
    std::vector<p4_symbolic::packet_synthesizer::SynthesizedPacket>&
        synthesized_packets,
    const pins_test::P4rtPortId& default_ingress_port,
    bool check_prediction_conformity) const {
  // Connect to the 4ward simulator for output prediction.
  auto channel =
      grpc::CreateChannel(sut_address_, grpc::InsecureChannelCredentials());
  auto stub = dataplane::Dataplane::NewStub(channel);

  dvaas::PacketTestVectorById test_vectors;

  for (size_t i = 0; i < synthesized_packets.size(); ++i) {
    auto& synthesized = synthesized_packets[i];
    int test_id = static_cast<int>(i) + 1;

    // Determine ingress port.
    std::string ingress_port = synthesized.ingress_port();
    if (ingress_port.empty()) {
      ingress_port = default_ingress_port.GetP4rtEncoding();
    }

    // Parse the raw packet, replace payload with test tag, re-serialize.
    packetlib::Packet parsed = packetlib::ParsePacket(synthesized.packet());
    parsed.set_payload(dvaas::MakeTestPacketTagFromUniqueId(
        test_id, "4ward hardcoded test"));
    // Recompute length/checksum fields after changing the payload.
    RETURN_IF_ERROR(packetlib::UpdateAllComputedFields(parsed).status());
    ASSIGN_OR_RETURN(std::string tagged_packet,
                     packetlib::SerializePacket(parsed));

    // Update the synthesized packet with the tagged version.
    synthesized.set_packet(tagged_packet);

    // Build the input side of the test vector.
    dvaas::PacketTestVector test_vector;
    auto* input = test_vector.mutable_input();
    input->set_type(dvaas::SwitchInput::DATAPLANE);
    auto* input_packet = input->mutable_packet();
    input_packet->set_port(ingress_port);
    input_packet->set_hex(absl::BytesToHexString(tagged_packet));
    *input_packet->mutable_parsed() = packetlib::ParsePacket(tagged_packet);

    // Generate output prediction by injecting into 4ward simulator.
    dataplane::InjectPacketRequest inject_request;
    auto* pkt = inject_request.mutable_packet();
    // Port is numeric in the 4ward proto.
    uint32_t port_num = 0;
    if (!absl::SimpleAtoi(ingress_port, &port_num)) {
      return absl::InvalidArgumentError(
          absl::StrCat("Non-numeric port: ", ingress_port));
    }
    pkt->set_ingress_port(port_num);
    pkt->set_payload(tagged_packet);

    grpc::ClientContext ctx;
    dataplane::InjectPacketResponse inject_response;
    auto status = stub->InjectPacket(&ctx, inject_request, &inject_response);
    if (!status.ok()) {
      return absl::InternalError(absl::StrCat(
          "Failed to inject packet into 4ward for prediction: ",
          status.error_message()));
    }

    // Build predicted output from the simulator response.
    auto* output = test_vector.add_acceptable_outputs();
    for (const auto& out_pkt : inject_response.output_packets()) {
      auto* predicted = output->add_packets();
      predicted->set_port(absl::StrCat(out_pkt.egress_port()));
      std::string out_bytes(out_pkt.payload().begin(),
                            out_pkt.payload().end());
      predicted->set_hex(absl::BytesToHexString(out_bytes));
      *predicted->mutable_parsed() = packetlib::ParsePacket(out_bytes);
    }

    // If no output packets, this is a predicted drop.
    if (inject_response.output_packets().empty()) {
      // Empty acceptable_outputs with no packets = expected drop.
      LOG(INFO) << "Test #" << test_id << ": predicted DROP";
    } else {
      LOG(INFO) << "Test #" << test_id << ": predicted "
                << inject_response.output_packets().size() << " output(s)";
    }

    test_vectors[test_id] = std::move(test_vector);
  }

  LOG(INFO) << "4ward backend: generated " << test_vectors.size()
            << " test vector(s) with output predictions";
  return test_vectors;
}

absl::StatusOr<pdpi::IrEntities> FourwardBackend::GetEntitiesToPuntAllPackets(
    const pdpi::IrP4Info& switch_p4info) const {
  // SAI P4 ACL entry that matches all packets and punts them to the controller.
  constexpr char kPuntAllEntities[] = R"pb(
    entities {
      table_entry {
        table_name: "acl_ingress_table"
        priority: 1
        action {
          name: "acl_trap"
          params {
            name: "qos_queue"
            value { str: "0x7" }
          }
        }
      }
    }
  )pb";

  pdpi::IrEntities entities;
  RETURN_IF_ERROR(gutil::ReadProtoFromString(kPuntAllEntities, &entities));
  return entities;
}

}  // namespace fourward
