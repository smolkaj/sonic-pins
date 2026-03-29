// E2E test: start a 4ward server, load SAI P4, install entries, inject
// packets, and verify output predictions and traces.

#include "fourward/fourward_oracle.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "gutil/proto.h"
#include "gutil/status_matchers.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_pdpi/ir.pb.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace dvaas {
namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;
using ::gutil::IsOk;
using ::testing::Not;

std::string RunfilePath(const std::string& path) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  if (runfiles == nullptr) {
    ADD_FAILURE() << "Failed to create Runfiles: " << error;
    return "";
  }
  return runfiles->Rlocation(path);
}

std::string ServerBinaryPath() {
  return RunfilePath("fourward/p4runtime/p4runtime_server");
}

p4::v1::ForwardingPipelineConfig LoadPipelineConfig() {
  std::string path = RunfilePath(
      "sonic_pins/fourward/sai_middleblock_fourward.p4rt.binpb");
  p4::v1::ForwardingPipelineConfig config;
  absl::Status status =
      gutil::ReadProtoFromFile(path, &config);
  EXPECT_THAT(status, IsOk()) << "Failed to load pipeline: " << path;
  return config;
}

TEST(FourwardOracleTest, CreateAndLoadPipeline) {
  std::string binary = ServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  p4::v1::ForwardingPipelineConfig config = LoadPipelineConfig();
  ASSERT_FALSE(config.p4info().tables().empty())
      << "Pipeline config has no tables";

  absl::StatusOr<std::unique_ptr<FourwardOracle>> oracle =
      FourwardOracle::Create(binary, config);
  ASSERT_THAT(oracle, IsOk());
  EXPECT_FALSE((*oracle)->ServerAddress().empty());
}

TEST(FourwardOracleTest, PredictDropsPacketWithNoEntries) {
  std::string binary = ServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  p4::v1::ForwardingPipelineConfig config = LoadPipelineConfig();
  absl::StatusOr<std::unique_ptr<FourwardOracle>> oracle =
      FourwardOracle::Create(binary, config);
  ASSERT_THAT(oracle, IsOk());

  // Inject a minimal Ethernet frame with no matching entries installed.
  // SAI P4 should drop it (no forwarding rules).
  std::string ethernet_frame(64, '\0');
  absl::StatusOr<PacketPrediction> prediction =
      (*oracle)->Predict(/*ingress_port=*/"1", ethernet_frame);
  ASSERT_THAT(prediction, IsOk());

  // With no entries, the packet should be dropped (no output packets).
  EXPECT_TRUE(prediction->output_packets.empty())
      << "Expected drop, got " << prediction->output_packets.size()
      << " output packets";

  // Trace should be non-empty.
  EXPECT_FALSE(prediction->trace_tree_textproto.empty());
}

TEST(FourwardOracleTest, PredictAllBatchProcessing) {
  std::string binary = ServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  p4::v1::ForwardingPipelineConfig config = LoadPipelineConfig();
  absl::StatusOr<std::unique_ptr<FourwardOracle>> oracle =
      FourwardOracle::Create(binary, config);
  ASSERT_THAT(oracle, IsOk());

  // Inject 10 packets in a batch.
  std::string ethernet_frame(64, '\0');
  std::vector<PacketInput> packets;
  for (int i = 0; i < 10; ++i) {
    packets.push_back({.ingress_port = "1", .payload = ethernet_frame});
  }

  absl::StatusOr<std::vector<PacketPrediction>> predictions =
      (*oracle)->PredictAll(packets);
  ASSERT_THAT(predictions, IsOk());
  EXPECT_EQ(predictions->size(), 10);

  for (const PacketPrediction& prediction : *predictions) {
    EXPECT_TRUE(prediction.output_packets.empty())
        << "Expected drop with no entries";
    EXPECT_FALSE(prediction.trace_tree_textproto.empty());
  }
}

}  // namespace
}  // namespace dvaas
