// E2E acceptance test: 4ward correctly predicts L3-forwarded packets.

#include <memory>
#include <string>
#include <vector>

#include "fourward/fourward_oracle.h"
#include "fourward/test_util.h"
#include "fourward/trace_summary.h"
#include "gtest/gtest.h"
#include "gutil/status_matchers.h"
#include "p4/v1/p4runtime.pb.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"

namespace dvaas {
namespace {

TEST(FourwardDvaasTest, Ipv4PacketIsForwardedToCorrectPort) {
  p4::v1::ForwardingPipelineConfig config = LoadSaiMiddleblock4wardConfig();
  ASSERT_FALSE(config.p4info().tables().empty());

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardOracle> oracle,
                       FourwardOracle::Create(config));
  ASSERT_OK(sai::EntryBuilder()
                .AddDisableVlanChecksEntry()
                .AddDisableIngressVlanChecksEntry()
                .AddDisableEgressVlanChecksEntry()
                .AddEntriesForwardingIpPacketsToGivenPort("1")
                .InstallDedupedEntities(oracle->Session()));

  std::string packet = SerializeTestPacket(R"pb(
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
  )pb");

  ASSERT_OK_AND_ASSIGN(
      std::vector<PacketPrediction> predictions,
      oracle->PredictAll({{.ingress_port = "0", .payload = packet}}));
  ASSERT_EQ(predictions.size(), 1);
  ASSERT_EQ(predictions[0].output_packets.size(), 1)
      << "Expected packet to be forwarded, but got dropped.\n"
      << SummarizeTrace(predictions[0].trace);
  EXPECT_EQ(predictions[0].output_packets[0].port, "1");
}

TEST(FourwardDvaasTest, UnroutablePacketIsDropped) {
  p4::v1::ForwardingPipelineConfig config = LoadSaiMiddleblock4wardConfig();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardOracle> oracle,
                       FourwardOracle::Create(config));

  std::string packet = SerializeTestPacket(R"pb(
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
  )pb");

  ASSERT_OK_AND_ASSIGN(
      std::vector<PacketPrediction> predictions,
      oracle->PredictAll({{.ingress_port = "0", .payload = packet}}));
  ASSERT_EQ(predictions.size(), 1);
  EXPECT_TRUE(predictions[0].output_packets.empty());
  EXPECT_GT(predictions[0].trace.events_size(), 0);
}

}  // namespace
}  // namespace dvaas
