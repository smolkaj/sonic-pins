#include "fourward/fourward_pins_switch.h"

#include <memory>

#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "gtest/gtest.h"
#include "gutil/status_matchers.h"

namespace dvaas {
namespace {

TEST(FourwardPinsSwitchTest, CreateSucceeds) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  EXPECT_FALSE(pins_switch.ChassisName().empty());
  EXPECT_EQ(pins_switch.DeviceId(), 1);
}

TEST(FourwardPinsSwitchTest, CreateWithCustomDeviceId) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create({.device_id = 42}));
  EXPECT_EQ(pins_switch.DeviceId(), 42);
}

TEST(FourwardPinsSwitchTest, CreateP4RuntimeStubSucceeds) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  EXPECT_OK(pins_switch.CreateP4RuntimeStub());
}

TEST(FourwardPinsSwitchTest, CreateGnmiStubSucceeds) {
  ASSERT_OK_AND_ASSIGN(FourwardPinsSwitch pins_switch,
                       FourwardPinsSwitch::Create());
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<gnmi::gNMI::StubInterface> gnmi_stub,
      pins_switch.CreateGnmiStub());
  EXPECT_NE(gnmi_stub, nullptr);
}

}  // namespace
}  // namespace dvaas
