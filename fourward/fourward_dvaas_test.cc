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

// End-to-end test: runs upstream DVaaS against two 4ward P4RuntimeServer
// instances, connected by a PacketBridge.
//
// Prerequisites:
//   1. Two 4ward P4RuntimeServer instances running:
//      - SUT:     bazel run //p4runtime:p4runtime_server -- --port 9559
//      - Control: bazel run //p4runtime:p4runtime_server -- --port 9560
//   2. Both instances should have no pipeline loaded (fresh start).
//
// The test will:
//   1. Load the SAI P4 pipeline on both switches.
//   2. Install L3 forwarding entries on the SUT.
//   3. Install punt-all entries on the control switch.
//   4. Start a PacketBridge between the two instances.
//   5. Run DVaaS with user-provided test vectors.
//   6. Assert that all tests pass.

#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "dvaas/dataplane_validation.h"
#include "dvaas/test_vector.pb.h"
#include "fourward/fourward_backend.h"
#include "fourward/fourward_switch.h"
#include "fourward/packet_bridge.h"
#include "gtest/gtest.h"
#include "gutil/gutil/proto.h"
#include "gutil/gutil/status.h"
#include "gutil/gutil/status_matchers.h"
#include "p4_infra/p4_pdpi/ir.h"
#include "p4_infra/p4_pdpi/p4_runtime_session.h"

ABSL_FLAG(std::string, sut_address, "localhost:9559",
          "Address of the 4ward SUT P4RuntimeServer.");
ABSL_FLAG(std::string, control_address, "localhost:9560",
          "Address of the 4ward control switch P4RuntimeServer.");
ABSL_FLAG(uint32_t, sut_device_id, 1, "P4Runtime device ID for the SUT.");
ABSL_FLAG(uint32_t, control_device_id, 2,
          "P4Runtime device ID for the control switch.");

namespace fourward {
namespace {

using ::gutil::IsOk;

// Builds a minimal test vector: an Ethernet frame sent on port 1, expected
// to be forwarded to port 2 by the SAI P4 pipeline.
dvaas::PacketTestVector BuildSimpleForwardingTestVector() {
  // Minimal Ethernet frame with IPv4 payload.
  // The hex encodes: dst_mac=ff:ff:ff:ff:ff:ff, src_mac=00:00:00:00:00:01,
  // ethertype=0x0800, then a minimal IPv4 header + 4 bytes of payload tagged
  // with test ID 1.
  constexpr char kTestVectorTextProto[] = R"pb(
    input {
      type: DATAPLANE
      packet {
        port: "1"
        hex: "ffffffffffff000000000001080045000018000100004011f96bc0a80001c0a800020000000000080001"
      }
    }
    acceptable_outputs {
      packets {
        port: "2"
        hex: "ffffffffffff000000000001080045000018000100003f11fa6bc0a80001c0a800020000000000080001"
      }
    }
  )pb";

  dvaas::PacketTestVector test_vector;
  CHECK(gutil::ReadProto(kTestVectorTextProto, &test_vector).ok());
  return test_vector;
}

TEST(FourwardDvaasTest, UpstreamDvaasValidation) {
  // Create the DataplaneValidator with our 4ward backend.
  auto backend = std::make_unique<FourwardBackend>();
  dvaas::DataplaneValidator validator(std::move(backend));

  // Create SwitchApi objects for SUT and control switch.
  ASSERT_OK_AND_ASSIGN(
      auto sut_session,
      pdpi::P4RuntimeSession::Create(
          absl::GetFlag(FLAGS_sut_address),
          grpc::InsecureChannelCredentials(),
          absl::GetFlag(FLAGS_sut_device_id)));
  ASSERT_OK_AND_ASSIGN(
      auto control_session,
      pdpi::P4RuntimeSession::Create(
          absl::GetFlag(FLAGS_control_address),
          grpc::InsecureChannelCredentials(),
          absl::GetFlag(FLAGS_control_device_id)));

  // Create gNMI stubs (4ward's gNMI stub server handles Get requests).
  auto sut_channel = grpc::CreateChannel(
      absl::GetFlag(FLAGS_sut_address), grpc::InsecureChannelCredentials());
  auto control_channel = grpc::CreateChannel(
      absl::GetFlag(FLAGS_control_address), grpc::InsecureChannelCredentials());

  dvaas::SwitchApi sut{
      .p4rt = std::move(sut_session),
      .gnmi = gnmi::gNMI::NewStub(sut_channel),
  };
  dvaas::SwitchApi control_switch{
      .p4rt = std::move(control_session),
      .gnmi = gnmi::gNMI::NewStub(control_channel),
  };

  // Start the PacketBridge to emulate physical links.
  PacketBridge bridge(absl::GetFlag(FLAGS_sut_address),
                      absl::GetFlag(FLAGS_control_address));
  ASSERT_THAT(bridge.Start(), IsOk());

  // Configure DVaaS params.
  dvaas::DataplaneValidationParams params;

  // Use pre-computed test vectors (skip automated synthesis).
  params.packet_test_vector_override = {BuildSimpleForwardingTestVector()};

  // Provide identity port map (SUT port X = control switch port X).
  params.mirror_testbed_port_map_override =
      dvaas::MirrorTestbedP4rtPortIdMap::CreateIdentityMap();

  // Disable failure analysis to avoid calling unimplemented backend methods.
  params.failure_enhancement_options.max_failures_to_attempt_to_replicate = 0;
  params.failure_enhancement_options.collect_packet_trace = false;
  params.failure_enhancement_options.max_number_of_failures_to_minimize = 0;
  params.reset_and_collect_counters = false;

  // Provide a dummy P4Specification to skip InferP4Specification.
  // TODO(4ward): Populate with real SAI P4 configs.
  params.specification_override = dvaas::P4Specification{};

  // Create a test environment.
  FourwardTestEnvironment env;

  // Run upstream DVaaS validation!
  LOG(INFO) << "Running upstream DVaaS validation against 4ward...";
  auto result = validator.ValidateDataplaneUsingExistingSwitchApis(
      sut, control_switch, env, params);

  bridge.Stop();
  LOG(INFO) << "PacketBridge forwarded " << bridge.PacketsForwarded()
            << " packets";

  ASSERT_THAT(result.status(), IsOk())
      << "DVaaS validation failed: " << result.status().message();
  LOG(INFO) << "DVaaS validation result: " << result->test_outcome_stats;
}

}  // namespace
}  // namespace fourward
