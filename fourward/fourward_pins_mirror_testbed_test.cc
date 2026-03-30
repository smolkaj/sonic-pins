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

#include "fourward/fourward_pins_mirror_testbed.h"

#include <memory>
#include <string>

#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gtest/gtest.h"
#include "gutil/io.h"
#include "gutil/status_matchers.h"
#include "gutil/testing.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_infra/p4_runtime/p4_runtime_session.h"
#include "p4_infra/p4_runtime/p4_runtime_session_extras.h"
#include "packetlib/packetlib.h"
#include "packetlib/packetlib.pb.h"
#include "sai_p4/instantiations/google/test_tools/test_entries.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace dvaas {
namespace {

using ::bazel::tools::cpp::runfiles::Runfiles;
using ::gutil::ParseProtoOrDie;
using fourward::dataplane::Dataplane;
using fourward::dataplane::InjectPacketRequest;
using fourward::dataplane::InjectPacketResponse;
using fourward::dataplane::SubscribeResultsRequest;
using fourward::dataplane::SubscribeResultsResponse;

TEST(FourwardPinsMirrorTestbedTest, CreateSucceeds) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                        FourwardPinsMirrorTestbed::Create());
  EXPECT_FALSE(testbed->Sut().ChassisName().empty());
  EXPECT_FALSE(testbed->ControlSwitch().ChassisName().empty());
  EXPECT_EQ(testbed->Sut().DeviceId(), 1);
  EXPECT_EQ(testbed->ControlSwitch().DeviceId(), 2);
}

TEST(FourwardPinsMirrorTestbedTest, P4RuntimeStubsConnect) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                        FourwardPinsMirrorTestbed::Create());
  EXPECT_OK(testbed->Sut().CreateP4RuntimeStub());
  EXPECT_OK(testbed->ControlSwitch().CreateP4RuntimeStub());
}

TEST(FourwardPinsMirrorTestbedTest, GnmiStubsConnect) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                        FourwardPinsMirrorTestbed::Create());
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<gnmi::gNMI::StubInterface> sut_gnmi,
      testbed->Sut().CreateGnmiStub());
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<gnmi::gNMI::StubInterface> control_gnmi,
      testbed->ControlSwitch().CreateGnmiStub());
  EXPECT_NE(sut_gnmi, nullptr);
  EXPECT_NE(control_gnmi, nullptr);
}

TEST(FourwardPinsMirrorTestbedTest, CustomDeviceIds) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                        FourwardPinsMirrorTestbed::Create(
                            /*sut_device_id=*/10, /*control_device_id=*/20));
  EXPECT_EQ(testbed->Sut().DeviceId(), 10);
  EXPECT_EQ(testbed->ControlSwitch().DeviceId(), 20);
}

TEST(FourwardPinsMirrorTestbedTest, BridgeForwardsPacketsBetweenInstances) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  ASSERT_NE(runfiles, nullptr) << "Failed to create Runfiles: " << error;
  ASSERT_OK_AND_ASSIGN(std::string pipeline_bytes, gutil::ReadFile(
      runfiles->Rlocation("_main/fourward/sai_middleblock_fourward.binpb")));
  p4::v1::ForwardingPipelineConfig config;
  ASSERT_TRUE(config.ParseFromString(pipeline_bytes));

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FourwardPinsMirrorTestbed> testbed,
                        FourwardPinsMirrorTestbed::Create());

  // Load pipeline on both instances.
  auto load_pipeline = [&](thinkit::Switch& sw) -> absl::Status {
    ASSIGN_OR_RETURN(std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub,
                     sw.CreateP4RuntimeStub());
    ASSIGN_OR_RETURN(std::unique_ptr<p4_runtime::P4RuntimeSession> session,
                     p4_runtime::P4RuntimeSession::Create(
                         std::move(stub), sw.DeviceId()));
    RETURN_IF_ERROR(p4_runtime::SetMetadataAndSetForwardingPipelineConfig(
        session.get(),
        p4::v1::SetForwardingPipelineConfigRequest::RECONCILE_AND_COMMIT,
        config));
    return absl::OkStatus();
  };
  ASSERT_OK(load_pipeline(testbed->Sut()));
  ASSERT_OK(load_pipeline(testbed->ControlSwitch()));

  // Install forwarding entries on SUT so injected packets egress on port 1.
  {
    ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<p4::v1::P4Runtime::StubInterface> stub,
        testbed->Sut().CreateP4RuntimeStub());
    ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<p4_runtime::P4RuntimeSession> session,
        p4_runtime::P4RuntimeSession::Create(
            std::move(stub), testbed->Sut().DeviceId()));
    ASSERT_OK(sai::EntryBuilder()
                  .AddDisableVlanChecksEntry()
                  .AddDisableIngressVlanChecksEntry()
                  .AddDisableEgressVlanChecksEntry()
                  .AddEntriesForwardingIpPacketsToGivenPort("1")
                  .InstallDedupedEntities(*session));
  }

  // Subscribe to results on the control switch BEFORE injecting into the SUT.
  std::shared_ptr<grpc::Channel> control_channel = grpc::CreateChannel(
      testbed->ControlSwitch().ChassisName(),
      grpc::InsecureChannelCredentials());
  std::unique_ptr<Dataplane::StubInterface> control_dataplane =
      Dataplane::NewStub(control_channel);
  grpc::ClientContext subscribe_ctx;
  std::unique_ptr<grpc::ClientReaderInterface<SubscribeResultsResponse>>
      reader = control_dataplane->SubscribeResults(
          &subscribe_ctx, SubscribeResultsRequest());

  SubscribeResultsResponse sub_response;
  ASSERT_TRUE(reader->Read(&sub_response));
  ASSERT_TRUE(sub_response.has_active());

  // Build and inject a test packet into the SUT on port 0.
  packetlib::Packet packet = ParseProtoOrDie<packetlib::Packet>(R"pb(
    headers {
      ethernet_header {
        ethernet_destination: "02:02:02:02:02:02"
        ethernet_source: "00:aa:bb:cc:dd:ee"
        ethertype: "0x0800"
      }
    }
    headers {
      ipv4_header {
        version: "0x4"
        ihl: "0x5"
        dscp: "0x00"
        ecn: "0x0"
        identification: "0x0000"
        flags: "0x0"
        fragment_offset: "0x0000"
        ttl: "0x40"
        protocol: "0x11"
        ipv4_source: "192.168.1.1"
        ipv4_destination: "10.1.2.3"
      }
    }
    headers {
      udp_header { source_port: "0x0000" destination_port: "0x0000" }
    }
  )pb");
  ASSERT_OK(packetlib::PadPacketToMinimumSize(packet));
  ASSERT_OK(packetlib::UpdateAllComputedFields(packet));
  ASSERT_OK_AND_ASSIGN(std::string payload, packetlib::SerializePacket(packet));

  {
    std::shared_ptr<grpc::Channel> sut_channel = grpc::CreateChannel(
        testbed->Sut().ChassisName(), grpc::InsecureChannelCredentials());
    std::unique_ptr<Dataplane::StubInterface> sut_dataplane =
        Dataplane::NewStub(sut_channel);
    grpc::ClientContext inject_ctx;
    InjectPacketRequest request;
    request.set_dataplane_ingress_port(0);
    request.set_payload(payload);
    InjectPacketResponse response;
    grpc::Status status =
        sut_dataplane->InjectPacket(&inject_ctx, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_FALSE(response.possible_outcomes().empty());
    ASSERT_FALSE(response.possible_outcomes(0).packets().empty())
        << "SUT dropped the packet — forwarding entries may not have taken "
           "effect.";
  }

  // The bridge should have forwarded the SUT's output to the control switch.
  ASSERT_TRUE(reader->Read(&sub_response))
      << "No result received on control switch — bridge did not forward the "
         "packet.";
  ASSERT_TRUE(sub_response.has_result());

  // The SUT egresses on port 1; the bridge resolves this to an interface
  // name ("Ethernet0" with P4RT ID 1 in the default FakeGnmiService config)
  // and finds the matching port on the control switch (also P4RT ID 1,
  // since both use the same default config).
  EXPECT_EQ(sub_response.result().input_packet().dataplane_ingress_port(), 1);

  subscribe_ctx.TryCancel();
}

}  // namespace
}  // namespace dvaas
