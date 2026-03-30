// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fourward/packet_bridge.h"

#include <memory>
#include <string>
#include <vector>

#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_server.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gtest/gtest.h"
#include "gutil/status_matchers.h"

namespace dvaas {
namespace {

// Helper: creates a gNMI stub connected to a FakeGnmiServer.
std::shared_ptr<gnmi::gNMI::StubInterface> GnmiStubFor(
    const FakeGnmiServer& server) {
  auto channel = grpc::CreateChannel(server.Address(),
                                     grpc::InsecureChannelCredentials());
  return gnmi::gNMI::NewStub(channel);
}

// Regression: two switches with the same default port config.
TEST(PacketBridgeTest, StartAndStopWithSameConfig) {
  ASSERT_OK_AND_ASSIGN(FourwardServer sut, FourwardServer::Start(1));
  ASSERT_OK_AND_ASSIGN(FourwardServer control, FourwardServer::Start(2));
  ASSERT_OK_AND_ASSIGN(auto sut_gnmi, FakeGnmiServer::Create());
  ASSERT_OK_AND_ASSIGN(auto control_gnmi, FakeGnmiServer::Create());

  PacketBridge bridge(sut.Address(), control.Address(),
                      GnmiStubFor(*sut_gnmi), GnmiStubFor(*control_gnmi));
  ASSERT_OK(bridge.Start());
  EXPECT_EQ(bridge.PacketsForwarded(), 0);
  EXPECT_EQ(bridge.InjectFailures(), 0);
  bridge.Stop();
}

// Two switches where "Ethernet0" has P4RT ID 1 on the SUT but P4RT ID 42 on
// the control switch. The bridge must route by interface name, not port number.
TEST(PacketBridgeTest, StartAndStopWithDifferentPortIds) {
  ASSERT_OK_AND_ASSIGN(FourwardServer sut, FourwardServer::Start(1));
  ASSERT_OK_AND_ASSIGN(FourwardServer control, FourwardServer::Start(2));

  // SUT: Ethernet0 -> P4RT ID 1
  std::vector<FakeInterface> sut_interfaces = {
      {.name = "Ethernet0", .p4rt_id = 1},
      {.name = "Ethernet1", .p4rt_id = 2},
  };
  // Control: Ethernet0 -> P4RT ID 42 (different!)
  std::vector<FakeInterface> control_interfaces = {
      {.name = "Ethernet0", .p4rt_id = 42},
      {.name = "Ethernet1", .p4rt_id = 43},
  };

  ASSERT_OK_AND_ASSIGN(auto sut_gnmi,
                        FakeGnmiServer::Create(sut_interfaces));
  ASSERT_OK_AND_ASSIGN(auto control_gnmi,
                        FakeGnmiServer::Create(control_interfaces));

  PacketBridge bridge(sut.Address(), control.Address(),
                      GnmiStubFor(*sut_gnmi), GnmiStubFor(*control_gnmi));
  ASSERT_OK(bridge.Start());
  EXPECT_EQ(bridge.PacketsForwarded(), 0);
  EXPECT_EQ(bridge.InjectFailures(), 0);
  bridge.Stop();
}

// Verify the static gNMI lookup helpers produce correct mappings.
TEST(PacketBridgeTest, InterfaceNameForPortFindsCorrectName) {
  std::vector<FakeInterface> interfaces = {
      {.name = "Ethernet0", .p4rt_id = 5},
      {.name = "Ethernet1", .p4rt_id = 10},
  };
  ASSERT_OK_AND_ASSIGN(auto gnmi_server,
                        FakeGnmiServer::Create(interfaces));
  auto stub = GnmiStubFor(*gnmi_server);

  // Port 5 (P4RT ID 5) -> "Ethernet0".
  ASSERT_OK_AND_ASSIGN(std::string name,
                        PacketBridge::InterfaceNameForPort(*stub, 5));
  EXPECT_EQ(name, "Ethernet0");

  // Port 10 (P4RT ID 10) -> "Ethernet1".
  ASSERT_OK_AND_ASSIGN(name, PacketBridge::InterfaceNameForPort(*stub, 10));
  EXPECT_EQ(name, "Ethernet1");

  // Port 99 -> not found.
  EXPECT_FALSE(PacketBridge::InterfaceNameForPort(*stub, 99).ok());
}

TEST(PacketBridgeTest, PortForInterfaceNameFindsCorrectPort) {
  std::vector<FakeInterface> interfaces = {
      {.name = "Ethernet0", .p4rt_id = 5},
      {.name = "Ethernet1", .p4rt_id = 10},
  };
  ASSERT_OK_AND_ASSIGN(auto gnmi_server,
                        FakeGnmiServer::Create(interfaces));
  auto stub = GnmiStubFor(*gnmi_server);

  // "Ethernet0" -> port 5.
  ASSERT_OK_AND_ASSIGN(uint32_t port,
                        PacketBridge::PortForInterfaceName(*stub, "Ethernet0"));
  EXPECT_EQ(port, 5);

  // "Ethernet1" -> port 10.
  ASSERT_OK_AND_ASSIGN(port,
                        PacketBridge::PortForInterfaceName(*stub, "Ethernet1"));
  EXPECT_EQ(port, 10);

  // "EthernetX" -> not found.
  EXPECT_FALSE(PacketBridge::PortForInterfaceName(*stub, "EthernetX").ok());
}

}  // namespace
}  // namespace dvaas
