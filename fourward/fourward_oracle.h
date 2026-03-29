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
//   ASSERT_OK_AND_ASSIGN(auto prediction,
//       oracle->Predict(ingress_port, packet_bytes));
//   // prediction.output_packets, prediction.trace_tree

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
  // Output packets with their egress ports (P4RT encoding).
  struct OutputPacket {
    std::string port;   // P4RT port ID (string encoding for SAI P4).
    std::string bytes;  // Raw packet bytes.
  };
  std::vector<OutputPacket> output_packets;

  // Structured trace of packet processing (4ward TraceTree in text proto).
  // We pass this as a string to avoid a proto dependency on 4ward's
  // simulator.proto in the DVaaS frontend. The conversion to DVaaS's
  // PacketTrace happens in trace_conversion.h.
  std::string trace_tree_textproto;
};

// Manages a 4ward server and provides packet output prediction.
class FourwardOracle {
 public:
  // Creates a FourwardOracle: starts a 4ward server subprocess and loads
  // the given pipeline config.
  static absl::StatusOr<std::unique_ptr<FourwardOracle>> Create(
      const std::string& server_binary_path,
      const p4::v1::ForwardingPipelineConfig& pipeline_config,
      uint64_t device_id = 1);

  // Installs table entries on the 4ward server. Can be called multiple times.
  absl::Status InstallEntities(const pdpi::IrEntities& ir_entities,
                               const pdpi::IrP4Info& ir_p4info);

  // Predicts the output of a packet injected on the given ingress port.
  // `ingress_port` uses P4RT encoding (string for SAI P4).
  absl::StatusOr<PacketPrediction> Predict(absl::string_view ingress_port,
                                           absl::string_view packet_bytes);

  // Returns the address of the underlying 4ward server.
  const std::string& ServerAddress() const { return server_.Address(); }

 private:
  FourwardOracle(FourwardServer server,
                 std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub,
                 uint64_t device_id);

  FourwardServer server_;
  std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub_;
  uint64_t device_id_;
  uint64_t election_id_ = 1;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_ORACLE_H_
