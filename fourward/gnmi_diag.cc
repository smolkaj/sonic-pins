#include <iostream>
#include <memory>
#include "grpcpp/grpcpp.h"
#include "proto/gnmi/gnmi.grpc.pb.h"
#include "proto/gnmi/gnmi.pb.h"
#include "lib/gnmi/gnmi_helper.h"

int main(int argc, char** argv) {
  std::string address = "localhost:9559";
  if (argc > 1) address = argv[1];

  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = gnmi::gNMI::NewStub(channel);

  // Test 1: raw STATE query
  {
    auto req_or = pins_test::BuildGnmiGetRequest("interfaces", gnmi::GetRequest::STATE);
    if (!req_or.ok()) {
      std::cerr << "Failed to build request: " << req_or.status() << std::endl;
      return 1;
    }
    gnmi::GetResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto grpc_status = stub->Get(&context, *req_or, &response);
    if (!grpc_status.ok()) {
      std::cerr << "STATE gRPC failed: " << grpc_status.error_message() << std::endl;
      return 1;
    }
    std::cout << "STATE response JSON: "
              << response.notification(0).update(0).val().json_ietf_val()
              << std::endl;
  }

  // Test 2: GetInterfacesAsProto (CONFIG)
  {
    auto result = pins_test::GetInterfacesAsProto(*stub, gnmi::GetRequest::CONFIG, absl::Seconds(5));
    if (!result.ok()) {
      std::cerr << "GetInterfacesAsProto CONFIG failed: " << result.status() << std::endl;
    } else {
      std::cout << "CONFIG interfaces count: " << result->interfaces_size() << std::endl;
      for (const auto& iface : result->interfaces()) {
        std::cout << "  " << iface.name() << " enabled=" << iface.config().enabled()
                  << " p4rt_id=" << iface.config().p4rt_id() << std::endl;
      }
    }
  }

  // Test 3: GetInterfacesAsProto (STATE)
  {
    auto result = pins_test::GetInterfacesAsProto(*stub, gnmi::GetRequest::STATE, absl::Seconds(5));
    if (!result.ok()) {
      std::cerr << "GetInterfacesAsProto STATE failed: " << result.status() << std::endl;
    } else {
      std::cout << "STATE interfaces count: " << result->interfaces_size() << std::endl;
      for (const auto& iface : result->interfaces()) {
        std::cout << "  " << iface.name()
                  << " state.enabled=" << iface.state().enabled()
                  << " state.oper_status=\"" << iface.state().oper_status() << "\""
                  << std::endl;
      }
    }
  }

  // Test 4: GetInterfaceToOperStatusMapOverGnmi
  {
    auto result = pins_test::GetInterfaceToOperStatusMapOverGnmi(*stub, absl::Seconds(5));
    if (!result.ok()) {
      std::cerr << "GetInterfaceToOperStatusMapOverGnmi failed: " << result.status() << std::endl;
    } else {
      std::cout << "Oper status map:" << std::endl;
      for (const auto& [name, status] : *result) {
        std::cout << "  " << name << " -> \"" << status << "\"" << std::endl;
      }
    }
  }

  // Test 5: CheckInterfaceOperStateOverGnmi (what PortsUp actually calls)
  {
    auto status = pins_test::CheckInterfaceOperStateOverGnmi(
        *stub, "UP", {}, false, absl::Seconds(5));
    if (!status.ok()) {
      std::cerr << "CheckInterfaceOperStateOverGnmi failed: " << status << std::endl;
    } else {
      std::cout << "CheckInterfaceOperStateOverGnmi: OK!" << std::endl;
    }
  }

  // Test 6: IsEnabledEthernetInterface on CONFIG
  {
    auto result = pins_test::GetInterfacesAsProto(*stub, gnmi::GetRequest::CONFIG, absl::Seconds(5));
    if (result.ok()) {
      std::cout << "IsEnabledEthernetInterface check:" << std::endl;
      for (const auto& iface : result->interfaces()) {
        bool is_enabled_ethernet = pins_test::IsEnabledEthernetInterface(iface);
        std::cout << "  " << iface.name() << " -> " << (is_enabled_ethernet ? "YES" : "NO") << std::endl;
      }
    }
  }

  std::cout << "All diagnostics complete." << std::endl;
  return 0;
}
