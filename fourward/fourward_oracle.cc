#include "fourward/fourward_oracle.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "fourward/fourward_server.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/status.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"

namespace fourward {
namespace {

using ::fourward::dataplane::Dataplane;
using ::fourward::dataplane::InjectPacketRequest;
using ::fourward::dataplane::InjectPacketsResponse;
using ::fourward::dataplane::ProcessPacketResult;
using ::fourward::dataplane::SubscribeResultsRequest;
using ::fourward::dataplane::SubscribeResultsResponse;

absl::Status BecomePrimary(p4::v1::P4Runtime::StubInterface& stub,
                           uint64_t device_id) {
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriterInterface<
      p4::v1::StreamMessageRequest, p4::v1::StreamMessageResponse>>
      stream = stub.StreamChannel(&context);

  p4::v1::StreamMessageRequest request;
  p4::v1::MasterArbitrationUpdate* arbitration =
      request.mutable_arbitration();
  arbitration->set_device_id(device_id);
  arbitration->mutable_election_id()->set_low(1);
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

  stream->WritesDone();
  return gutil::GrpcStatusToAbslStatus(stream->Finish());
}

PacketPrediction ResultToPrediction(const ProcessPacketResult& result) {
  PacketPrediction prediction;
  for (const ::fourward::dataplane::PacketSet& outcome :
       result.possible_outcomes()) {
    for (const ::fourward::dataplane::OutputPacket& packet :
         outcome.packets()) {
      prediction.output_packets.push_back({
          .port = std::string(packet.p4rt_egress_port()),
          .bytes = std::string(packet.payload()),
      });
    }
    // TODO: Return all outcomes for non-deterministic programs.
    break;
  }
  if (result.has_trace()) {
    prediction.trace_tree_textproto = result.trace().DebugString();
  }
  return prediction;
}

}  // namespace

FourwardOracle::FourwardOracle(FourwardServer server,
                               std::shared_ptr<grpc::Channel> channel,
                               uint64_t device_id)
    : server_(std::move(server)),
      channel_(std::move(channel)),
      device_id_(device_id) {}

absl::StatusOr<std::unique_ptr<FourwardOracle>> FourwardOracle::Create(
    const std::string& server_binary_path,
    const p4::v1::ForwardingPipelineConfig& pipeline_config,
    uint64_t device_id) {
  absl::StatusOr<FourwardServer> server =
      FourwardServer::Start(server_binary_path, device_id);
  if (!server.ok()) return server.status();

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub =
      p4::v1::P4Runtime::NewStub(channel);

  absl::Status status = BecomePrimary(*stub, device_id);
  if (!status.ok()) return status;

  // Load the pipeline.
  p4::v1::SetForwardingPipelineConfigRequest request;
  request.set_device_id(device_id);
  request.mutable_election_id()->set_low(1);
  request.set_action(
      p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT);
  *request.mutable_config() = pipeline_config;

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(60));
  p4::v1::SetForwardingPipelineConfigResponse response;
  grpc::Status rpc_status =
      stub->SetForwardingPipelineConfig(&context, request, &response);
  if (!rpc_status.ok()) {
    return gutil::GrpcStatusToAbslStatus(rpc_status);
  }

  return std::unique_ptr<FourwardOracle>(
      new FourwardOracle(std::move(*server), std::move(channel), device_id));
}

absl::Status FourwardOracle::InstallEntities(
    const pdpi::IrEntities& ir_entities, const pdpi::IrP4Info& ir_p4info) {
  // TODO: Convert IR entities to PI via PDPI and install via Write RPC.
  return absl::UnimplementedError(
      "InstallEntities not yet implemented — needs PDPI IR→PI conversion");
}

absl::StatusOr<PacketPrediction> FourwardOracle::Predict(
    absl::string_view ingress_port, absl::string_view packet_bytes) {
  std::vector<PacketInput> inputs = {{
      .ingress_port = std::string(ingress_port),
      .payload = std::string(packet_bytes),
  }};
  absl::StatusOr<std::vector<PacketPrediction>> results = PredictAll(inputs);
  if (!results.ok()) return results.status();
  if (results->empty()) {
    return absl::InternalError("PredictAll returned no results");
  }
  return std::move((*results)[0]);
}

absl::StatusOr<std::vector<PacketPrediction>> FourwardOracle::PredictAll(
    const std::vector<PacketInput>& packets) {
  if (packets.empty()) return std::vector<PacketPrediction>{};

  std::unique_ptr<Dataplane::StubInterface> stub =
      Dataplane::NewStub(channel_);

  // Subscribe to results before injecting — ensures no results are missed.
  grpc::ClientContext results_context;
  std::unique_ptr<grpc::ClientReaderInterface<SubscribeResultsResponse>>
      results_reader = stub->SubscribeResults(
          &results_context, SubscribeResultsRequest());

  SubscribeResultsResponse response;
  if (!results_reader->Read(&response) || !response.has_active()) {
    return absl::InternalError("SubscribeResults did not confirm activation");
  }

  // Stream all packets via InjectPackets (processed concurrently by server).
  {
    grpc::ClientContext context;
    InjectPacketsResponse response;
    std::unique_ptr<grpc::ClientWriterInterface<InjectPacketRequest>> writer =
        stub->InjectPackets(&context, &response);

    for (const PacketInput& packet : packets) {
      InjectPacketRequest request;
      request.set_p4rt_ingress_port(packet.ingress_port);
      request.set_payload(packet.payload);
      if (!writer->Write(request)) {
        return absl::InternalError("Failed to write to InjectPackets stream");
      }
    }
    writer->WritesDone();

    grpc::Status status = writer->Finish();
    if (!status.ok()) {
      return gutil::GrpcStatusToAbslStatus(status);
    }
  }

  // Collect one result per injected packet.
  std::vector<PacketPrediction> predictions;
  predictions.reserve(packets.size());
  while (results_reader->Read(&response)) {
    if (!response.has_result()) continue;
    predictions.push_back(ResultToPrediction(response.result()));
    if (predictions.size() == packets.size()) break;
  }
  results_context.TryCancel();

  if (predictions.size() != packets.size()) {
    return absl::InternalError(absl::StrCat(
        "Expected ", packets.size(), " results, got ", predictions.size()));
  }
  return predictions;
}

}  // namespace fourward
