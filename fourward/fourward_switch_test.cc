// Unit tests for FourwardSwitch.

#include "fourward/fourward_switch.h"

#include <cstdint>
#include <string>

#include "gtest/gtest.h"

namespace dvaas {
namespace {

TEST(FourwardSwitchTest, ChassisNameIsP4rtAddress) {
  FourwardSwitch fourward_switch("localhost:9559", "localhost:9339",
                                 /*device_id=*/1);
  EXPECT_EQ(fourward_switch.ChassisName(), "localhost:9559");
}

TEST(FourwardSwitchTest, DeviceIdIsStored) {
  FourwardSwitch fourward_switch("localhost:9559", "localhost:9339",
                                 /*device_id=*/42);
  EXPECT_EQ(fourward_switch.DeviceId(), 42);
}

TEST(FourwardSwitchTest, CreateP4RuntimeStubSucceeds) {
  FourwardSwitch fourward_switch("localhost:9559", "localhost:9339",
                                 /*device_id=*/1);
  // Stub creation should succeed even without a running server — the
  // channel is lazy.
  EXPECT_TRUE(fourward_switch.CreateP4RuntimeStub().ok());
}

TEST(FourwardSwitchTest, CreateGnmiStubSucceeds) {
  FourwardSwitch fourward_switch("localhost:9559", "localhost:9339",
                                 /*device_id=*/1);
  EXPECT_TRUE(fourward_switch.CreateGnmiStub().ok());
}

TEST(FourwardSwitchTest, P4rtAndGnmiCanUseDifferentAddresses) {
  FourwardSwitch fourward_switch("localhost:1111", "localhost:2222",
                                 /*device_id=*/1);
  // ChassisName is the P4RT address, not the gNMI address.
  EXPECT_EQ(fourward_switch.ChassisName(), "localhost:1111");
}

}  // namespace
}  // namespace dvaas
