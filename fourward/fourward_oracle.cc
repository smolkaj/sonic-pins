#include "fourward/fourward_oracle.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "fourward/fourward_server.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"

namespace fourward {
namespace {

// Establishes P4Runtime primary arbitration on the given stub.
absl::Status BecomePrimary(p4::v1::P4Runtime::StubInterface& stub,
                           uint64_t device_id, uint64_t election_id) {
  grpc::ClientContext context;
  auto stream = stub.StreamChannel(&context);

  p4::v1::StreamMessageRequest request;
  auto* arb = request.mutable_arbitration();
  arb->set_device_id(device_id);
  arb->mutable_election_id()->set_low(election_id);

  if (!stream->Write(request)) {
    return absl::InternalError("Failed to send arbitration request");
  }

  p4::v1::StreamMessageResponse response;
  if (!stream->Read(&response)) {
    return absl::InternalError("Failed to read arbitration response");
  }

  if (!response.has_arbitration() ||
      response.arbitration().status().code() != 0) {
    return absl::InternalError("Arbitration failed");
  }

  // Keep the stream alive by not closing it — but we need to for our use
  // pattern. The 4ward server maintains the primary status even after the
  // stream closes, as long as no other client takes over.
  stream->WritesDone();
  stream->Finish();

  return absl::OkStatus();
}

std::unique_ptr<grpc::ClientContext> WithDeadline(int seconds) {
  auto ctx = std::make_unique<grpc::ClientContext>();
  ctx->set_deadline(std::chrono::system_clock::now() +
                    std::chrono::seconds(seconds));
  return ctx;
}

}  // namespace

FourwardOracle::FourwardOracle(
    FourwardServer server,
    std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub,
    uint64_t device_id)
    : server_(std::move(server)),
      stub_(std::move(stub)),
      device_id_(device_id) {}

absl::StatusOr<std::unique_ptr<FourwardOracle>> FourwardOracle::Create(
    const std::string& server_binary_path,
    const p4::v1::ForwardingPipelineConfig& pipeline_config,
    uint64_t device_id) {
  // Start the 4ward server subprocess.
  auto server = FourwardServer::Start(server_binary_path, device_id);
  if (!server.ok()) return server.status();

  auto channel = grpc::CreateChannel(server->Address(),
                                     grpc::InsecureChannelCredentials());
  auto stub = p4::v1::P4Runtime::NewStub(channel);

  // Become primary controller.
  auto status = BecomePrimary(*stub, device_id, /*election_id=*/1);
  if (!status.ok()) return status;

  // Load the pipeline.
  {
    p4::v1::SetForwardingPipelineConfigRequest req;
    req.set_device_id(device_id);
    req.mutable_election_id()->set_low(1);
    req.set_action(
        p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT);
    *req.mutable_config() = pipeline_config;

    auto ctx = WithDeadline(60);
    p4::v1::SetForwardingPipelineConfigResponse resp;
    auto rpc_status = stub->SetForwardingPipelineConfig(ctx.get(), req, &resp);
    if (!rpc_status.ok()) {
      return absl::InternalError(absl::StrCat(
          "SetForwardingPipelineConfig failed: ", rpc_status.error_message()));
    }
  }

  return std::unique_ptr<FourwardOracle>(
      new FourwardOracle(std::move(*server), std::move(stub), device_id));
}

absl::Status FourwardOracle::InstallEntities(
    const pdpi::IrEntities& ir_entities, const pdpi::IrP4Info& ir_p4info) {
  // TODO: Convert IR entities to PI entities via PDPI and install
  // via P4Runtime Write RPC. For now, this is a stub.
  return absl::UnimplementedError(
      "InstallEntities not yet implemented — needs PDPI IR→PI conversion");
}

absl::StatusOr<PacketPrediction> FourwardOracle::Predict(
    absl::string_view ingress_port, absl::string_view packet_bytes) {
  auto channel = grpc::CreateChannel(server_.Address(),
                                     grpc::InsecureChannelCredentials());
  auto dataplane_stub =
      ::fourward::dataplane::Dataplane::NewStub(channel);

  ::fourward::dataplane::InjectPacketRequest request;
  request.set_p4rt_ingress_port(
      std::string(ingress_port.data(), ingress_port.size()));
  request.set_payload(
      std::string(packet_bytes.data(), packet_bytes.size()));

  auto ctx = WithDeadline(30);
  ::fourward::dataplane::InjectPacketResponse response;
  auto status = dataplane_stub->InjectPacket(ctx.get(), request, &response);
  if (!status.ok()) {
    return absl::InternalError(
        absl::StrCat("InjectPacket failed: ", status.error_message()));
  }

  PacketPrediction prediction;
  for (const auto& outcome : response.possible_outcomes()) {
    for (const auto& pkt : outcome.packets()) {
      prediction.output_packets.push_back({
          .port = std::string(pkt.p4rt_egress_port()),
          .bytes = std::string(pkt.payload()),
      });
    }
    // TODO: Support multiple outcomes for non-deterministic programs.
    break;
  }

  if (response.has_trace()) {
    prediction.trace_tree_textproto = response.trace().DebugString();
  }

  return prediction;
}

}  // namespace fourward
