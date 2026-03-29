// Output and trace prediction via a 4ward P4Runtime server.
//
// FourwardOracle manages a 4ward server subprocess, loads a pipeline, installs
// table entries, and predicts packet outputs with traces. It is the building
// block for 4ward integration in the DVaaS frontend.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(auto oracle,
//       FourwardOracle::Create(binary_path, fourward_config));
//   ASSERT_OK(oracle->InstallEntities(ir_entities, ir_p4info));
//
//   // Single packet:
//   ASSERT_OK_AND_ASSIGN(auto prediction,
//       oracle->Predict(ingress_port, packet_bytes));
//
//   // Batch (streaming — much faster for many packets):
//   ASSERT_OK_AND_ASSIGN(auto predictions,
//       oracle->PredictAll(packets));

#ifndef PINS_FOURWARD_FOURWARD_ORACLE_H_
#define PINS_FOURWARD_FOURWARD_ORACLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "fourward/fourward_server.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_pdpi/ir.pb.h"

namespace fourward {

// Predicted output for a single injected packet.
struct PacketPrediction {
  struct OutputPacket {
    std::string port;   // P4RT port ID (string encoding for SAI P4).
    std::string bytes;  // Raw packet bytes.
  };
  std::vector<OutputPacket> output_packets;

  // Structured trace of packet processing (4ward TraceTree in text proto).
  // Passed as a string to avoid pulling 4ward's simulator.proto into the
  // DVaaS frontend. Conversion to DVaaS PacketTrace happens separately.
  std::string trace_tree_textproto;
};

// A packet to predict, with its ingress port (P4RT encoding).
struct PacketInput {
  std::string ingress_port;  // P4RT port ID.
  std::string payload;       // Raw packet bytes.
};

// Manages a 4ward server and provides packet output prediction.
class FourwardOracle {
 public:
  // Starts a 4ward server subprocess and loads the given pipeline config.
  static absl::StatusOr<std::unique_ptr<FourwardOracle>> Create(
      const std::string& server_binary_path,
      const p4::v1::ForwardingPipelineConfig& pipeline_config,
      uint64_t device_id = 1);

  // Installs table entries on the 4ward server. Can be called multiple times.
  absl::Status InstallEntities(const pdpi::IrEntities& ir_entities,
                               const pdpi::IrP4Info& ir_p4info);

  // Predicts the output of a single packet. Convenience wrapper around
  // PredictAll for one-off use.
  absl::StatusOr<PacketPrediction> Predict(absl::string_view ingress_port,
                                           absl::string_view packet_bytes);

  // Predicts outputs for a batch of packets using the streaming InjectPackets
  // + SubscribeResults RPCs. Results are returned in input order.
  absl::StatusOr<std::vector<PacketPrediction>> PredictAll(
      const std::vector<PacketInput>& packets);

  const std::string& ServerAddress() const { return server_.Address(); }

 private:
  FourwardOracle(FourwardServer server,
                 std::shared_ptr<grpc::Channel> channel,
                 uint64_t device_id);

  FourwardServer server_;
  std::shared_ptr<grpc::Channel> channel_;
  uint64_t device_id_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_ORACLE_H_
