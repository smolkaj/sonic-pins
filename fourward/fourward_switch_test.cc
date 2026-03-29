// Unit tests for FourwardSwitch.

#include "fourward/fourward_switch.h"

#include <string>

#include "fourward/fourward_server.h"
#include "gtest/gtest.h"

namespace dvaas {
namespace {

TEST(FourwardSwitchTest, ChassisNameIsServerAddress) {
  std::string binary = FourwardServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  absl::StatusOr<FourwardServer> server = FourwardServer::Start(binary);
  ASSERT_TRUE(server.ok()) << server.status();

  std::string expected_address = server->Address();
  uint64_t expected_device_id = server->DeviceId();

  FourwardSwitch fourward_switch(std::move(*server), "localhost:9339");
  EXPECT_EQ(fourward_switch.ChassisName(), expected_address);
  EXPECT_EQ(fourward_switch.DeviceId(), expected_device_id);
}

TEST(FourwardSwitchTest, CreateP4RuntimeStubSucceeds) {
  std::string binary = FourwardServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  absl::StatusOr<FourwardServer> server = FourwardServer::Start(binary);
  ASSERT_TRUE(server.ok()) << server.status();

  FourwardSwitch fourward_switch(std::move(*server), "localhost:9339");
  EXPECT_TRUE(fourward_switch.CreateP4RuntimeStub().ok());
}

TEST(FourwardSwitchTest, CreateGnmiStubSucceeds) {
  std::string binary = FourwardServerBinaryPath();
  ASSERT_FALSE(binary.empty());

  absl::StatusOr<FourwardServer> server = FourwardServer::Start(binary);
  ASSERT_TRUE(server.ok()) << server.status();

  FourwardSwitch fourward_switch(std::move(*server), "localhost:9339");
  EXPECT_TRUE(fourward_switch.CreateGnmiStub().ok());
}

}  // namespace
}  // namespace dvaas
