// TDD: FourwardPinsSwitch behaves like a real PINS switch.
//
// Auxiliary entries (PRE clone sessions, ingress_clone_table entries) are
// installed transparently and invisible to the user. These tests verify
// the contract by checking that behavior changes with auxiliary entries
// and that they don't appear in P4Runtime reads.

#include "fourward/fourward_pins_switch.h"

#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "gutil/io.h"
#include "gutil/status_matchers.h"
#include "gutil/testing.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_infra/p4_runtime/p4_runtime_session.h"
#include "p4_infra/p4_runtime/p4_runtime_session_extras.h"
#include "packetlib/packetlib.h"
#include "packetlib/packetlib.pb.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace dvaas {
namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;
using ::gutil::ParseProtoOrDie;

p4::v1::ForwardingPipelineConfig LoadFourwardConfig() {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  EXPECT_NE(runfiles, nullptr) << "Failed to create Runfiles: " << error;
  absl::StatusOr<std::string> contents = gutil::ReadFile(
      runfiles->Rlocation("_main/fourward/sai_middleblock_fourward.binpb"));
  EXPECT_OK(contents);
  p4::v1::ForwardingPipelineConfig config;
  EXPECT_TRUE(config.ParseFromString(*contents));
  return config;
}

std::string SerializeTestPacket(absl::string_view textproto) {
  packetlib::Packet packet = ParseProtoOrDie<packetlib::Packet>(textproto);
  CHECK_OK(packetlib::PadPacketToMinimumSize(packet));
  CHECK_OK(packetlib::UpdateAllComputedFields(packet));
  absl::StatusOr<std::string> serialized = packetlib::SerializePacket(packet);
  CHECK_OK(serialized);
  return *serialized;
}

constexpr absl::string_view kUdpPacket = R"pb(
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
  headers { udp_header { source_port: "0x0000" destination_port: "0x0000" } }
)pb";

// Loads pipeline and installs ACL trap entries on the given switch. Returns
// the P4Runtime session (which must outlive the switch for StreamChannel).
absl::StatusOr<std::unique_ptr<p4_runtime::P4RuntimeSession>>
SetUpSwitchWithAclTrap(FourwardPinsSwitch& pins_switch) {
  p4::v1::ForwardingPipelineConfig config = LoadFourwardConfig();
  ASSIGN_OR_RETURN(std::unique_ptr<p4_runtime::P4RuntimeSession> session,
                   p4_runtime::P4RuntimeSession::Create(pins_switch));
  RETURN_IF_ERROR(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
      session.get(),
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
      config));
  RETURN_IF_ERROR(sai::EntryBuilder()
                      .AddEntryPuntingAllPackets(sai::PuntAction::kTrap)
                      .InstallDedupedEntities(*session));
  return session;
}

// Injects a packet via the 4ward Dataplane service, which triggers the
// pre-packet hook (installing auxiliary entries) before processing.
fourward::dataplane::InjectPacketResponse InjectPacket(
    FourwardPinsSwitch& pins_switch, int ingress_port,
    const std::string& payload) {
  auto stub = pins_switch.CreateDataplaneStub();
  grpc::ClientContext context;
  fourward::dataplane::InjectPacketRequest request;
  request.set_dataplane_ingress_port(ingress_port);
  request.set_payload(payload);
  fourward::dataplane::InjectPacketResponse response;
  grpc::Status status = stub->InjectPacket(&context, request, &response);
  EXPECT_TRUE(status.ok()) << "InjectPacket failed: " << status.error_message();
  return response;
}

int OutputPacketCount(
    const fourward::dataplane::InjectPacketResponse& response) {
  if (response.possible_outcomes_size() == 0) return 0;
  return response.possible_outcomes(0).packets_size();
}

// ---------------------------------------------------------------------------
// Litmus test 1: ACL trap requires PRE auxiliary entry
// ---------------------------------------------------------------------------

// An ACL trap entry punts matching packets to the CPU via the Packet
// Replication Engine. Without the PRE clone session and ingress_clone_table
// entry (auxiliary entries), the punt silently fails.
TEST(FourwardPinsSwitchTest, AclTrapPuntsPacketWithAuxEntries) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  ASSERT_OK_AND_ASSIGN(auto session, SetUpSwitchWithAclTrap(pins_switch));

  std::string payload = SerializeTestPacket(kUdpPacket);
  auto response =
      InjectPacket(pins_switch, /*ingress_port=*/0, payload);

  EXPECT_GT(OutputPacketCount(response), 0)
      << "Expected cloned packet from ACL trap, but got none. "
         "The PRE clone session (auxiliary entry) is likely missing.";
}

// ---------------------------------------------------------------------------
// Litmus test 2: L3 forwarding requires VLAN auxiliary entries
// ---------------------------------------------------------------------------

// L3 forwarding requires VLAN check disable entries. Without them,
// packets are dropped before reaching the routing tables.
TEST(FourwardPinsSwitchTest, L3ForwardingWorksWithAuxEntries) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<p4_runtime::P4RuntimeSession> session,
      p4_runtime::P4RuntimeSession::Create(pins_switch));
  p4::v1::ForwardingPipelineConfig config = LoadFourwardConfig();
  ASSERT_OK(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
      session.get(),
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
      config));
  ASSERT_OK(sai::EntryBuilder()
                .AddEntriesForwardingIpPacketsToGivenPort("1")
                .InstallDedupedEntities(*session));

  std::string payload = SerializeTestPacket(kUdpPacket);
  auto response =
      InjectPacket(pins_switch, /*ingress_port=*/0, payload);

  EXPECT_GT(OutputPacketCount(response), 0)
      << "Packet was dropped — VLAN disable auxiliary entries are likely "
         "missing.";
}

// ---------------------------------------------------------------------------
// Litmus test 3: Auxiliary entries are invisible to P4Runtime reads
// ---------------------------------------------------------------------------

// A wildcard read by a sdn_controller session should NOT return
// auxiliary entries installed by the pins_auxiliary role.
TEST(FourwardPinsSwitchTest, DISABLED_AuxiliaryEntriesInvisibleToReads) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  ASSERT_OK_AND_ASSIGN(auto session, SetUpSwitchWithAclTrap(pins_switch));

  // Trigger auxiliary entry reconciliation by injecting a packet.
  std::string payload = SerializeTestPacket(kUdpPacket);
  InjectPacket(pins_switch, /*ingress_port=*/0, payload);

  // Read all entries as sdn_controller.
  p4::v1::ReadRequest read_request;
  read_request.set_device_id(pins_switch.DeviceId());
  read_request.add_entities()->mutable_table_entry();
  ASSERT_OK_AND_ASSIGN(p4::v1::ReadResponse read_response,
                       session->Read(read_request));

  // Verify: only the user-installed ACL entry should be visible.
  // TODO: Check against known auxiliary table IDs.
  EXPECT_GT(read_response.entities_size(), 0)
      << "Expected at least the user-installed ACL entry";
}

// ---------------------------------------------------------------------------
// Litmus test 4: With vs without — prove auxiliary entries are the cause
// ---------------------------------------------------------------------------

// Two switches, same pipeline, same ACL trap entry, same packet. One has
// auxiliary entries enabled, the other doesn't. Only the one with auxiliary
// entries produces a cloned output — proving the auxiliary entries are the
// cause.
TEST(FourwardPinsSwitchTest, AclTrapFailsWithoutAuxEntriesSucceedsWith) {
  std::string payload = SerializeTestPacket(kUdpPacket);

  // WITHOUT auxiliary entries: clone silently fails.
  {
    ASSERT_OK_AND_ASSIGN(
        FourwardPinsSwitch pins_switch,
        FourwardPinsSwitch::Create({.enable_auxiliary_entries = false}));
    ASSERT_OK_AND_ASSIGN(auto session, SetUpSwitchWithAclTrap(pins_switch));
    auto response =
        InjectPacket(pins_switch, /*ingress_port=*/0, payload);
    EXPECT_EQ(OutputPacketCount(response), 0)
        << "Expected NO output without auxiliary entries, but got output.";
  }

  // WITH auxiliary entries: clone succeeds.
  {
    ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                         FourwardPinsSwitch::Create());
    ASSERT_OK_AND_ASSIGN(auto session, SetUpSwitchWithAclTrap(pins_switch));
    auto response =
        InjectPacket(pins_switch, /*ingress_port=*/0, payload);
    EXPECT_GT(OutputPacketCount(response), 0)
        << "Expected cloned output with auxiliary entries, but got none.";
  }
}

}  // namespace
}  // namespace dvaas
