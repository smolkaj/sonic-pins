// Output and trace prediction via a 4ward P4Runtime server.
//
// FourwardOracle manages a 4ward server subprocess, loads a pipeline, installs
// table entries, and predicts packet outputs with traces. It is the building
// block for 4ward integration in the DVaaS frontend.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardOracle> oracle,
//       FourwardOracle::Create(binary_path, fourward_config));
//   ASSERT_OK(oracle->InstallIrEntities(ir_entities));
//   ASSERT_OK_AND_ASSIGN(std::vector<PacketPrediction> predictions,
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
#include "grpcpp/channel.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_infra/p4_runtime/p4_runtime_session.h"
#include "p4_pdpi/ir.pb.h"
#include "simulator.pb.h"

namespace dvaas {

// Predicted output for a single injected packet.
struct PacketPrediction {
  struct OutputPacket {
    std::string port;   // P4RT port ID (string encoding for SAI P4).
    std::string bytes;  // Raw packet bytes.
  };
  std::vector<OutputPacket> output_packets;

  // Structured trace of packet processing.
  fourward::sim::TraceTree trace;
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
      const p4::v1::ForwardingPipelineConfig& pipeline_config,
      uint32_t device_id = 1);

  // Installs IR entities on the 4ward server. Reads P4Info from the server
  // for IR-to-PI translation.
  absl::Status InstallIrEntities(const pdpi::IrEntities& ir_entities);

  // Predicts the output of a single packet.
  absl::StatusOr<PacketPrediction> Predict(const PacketInput& packet);

  // Predicts outputs for a batch of packets using the streaming InjectPackets
  // + SubscribeResults RPCs. Results are returned in input order.
  absl::StatusOr<std::vector<PacketPrediction>> PredictAll(
      const std::vector<PacketInput>& packets);

  const std::string& ServerAddress() const { return server_.Address(); }

  // Exposes the P4Runtime session for direct use (e.g. by DVaaS).
  p4_runtime::P4RuntimeSession& Session() { return *session_; }

 private:
  FourwardOracle(FourwardServer server,
                 std::shared_ptr<grpc::Channel> channel,
                 std::unique_ptr<p4_runtime::P4RuntimeSession> session);

  FourwardServer server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<p4_runtime::P4RuntimeSession> session_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FOURWARD_ORACLE_H_
