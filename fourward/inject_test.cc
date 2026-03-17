#include <iostream>
#include "absl/log/log.h"
#include "fourward/dataplane.grpc.pb.h"
#include "fourward/dataplane.pb.h"
#include "grpcpp/grpcpp.h"

int main() {
  auto channel = grpc::CreateChannel("localhost:9559", grpc::InsecureChannelCredentials());
  auto stub = fourward::dataplane::Dataplane::NewStub(channel);

  fourward::dataplane::InjectPacketRequest req;
  auto* pkt = req.mutable_packet();
  pkt->set_ingress_port(1);
  // Minimal Ethernet + IPv4 packet
  std::string payload = std::string(64, '\x00');
  payload[12] = 0x08; payload[13] = 0x00;  // ethertype IPv4
  payload[14] = 0x45;  // IPv4 version+IHL
  payload[22] = 0x40;  // TTL
  payload[23] = 0x11;  // UDP
  pkt->set_payload(payload);

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
  fourward::dataplane::InjectPacketResponse resp;

  LOG(INFO) << "Calling InjectPacket...";
  auto status = stub->InjectPacket(&ctx, req, &resp);
  if (status.ok()) {
    LOG(INFO) << "OK! Got " << resp.output_packets_size() << " output packets";
  } else {
    LOG(ERROR) << "FAILED: " << status.error_code() << " " << status.error_message();
  }
  return status.ok() ? 0 : 1;
}
