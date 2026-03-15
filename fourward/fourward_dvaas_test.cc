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
//   1. Create a FourwardMirrorTestbed backed by the two 4ward instances.
//   2. Start a PacketBridge to emulate physical links between them.
//   3. Run DVaaS validation with user-provided test vectors.
//   4. Assert that all tests pass.

#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dvaas/dataplane_validation.h"
#include "dvaas/test_vector.pb.h"
#include "fourward/fourward_backend.h"
#include "fourward/fourward_switch.h"
#include "fourward/packet_bridge.h"
#include "gtest/gtest.h"
#include "gutil/gutil/status_matchers.h"
#include "gutil/gutil/testing.h"

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
  return gutil::ParseProtoOrDie<dvaas::PacketTestVector>(R"pb(
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
  )pb");
}

TEST(FourwardDvaasTest, UpstreamDvaasValidation) {
  // Create the DataplaneValidator with our 4ward backend.
  auto backend = std::make_unique<FourwardBackend>();
  dvaas::DataplaneValidator validator(std::move(backend));

  // Create a MirrorTestbed backed by two 4ward instances.
  FourwardMirrorTestbed testbed(
      absl::GetFlag(FLAGS_sut_address), absl::GetFlag(FLAGS_sut_device_id),
      absl::GetFlag(FLAGS_control_address),
      absl::GetFlag(FLAGS_control_device_id));

  // Start the PacketBridge to emulate physical links between the two instances.
  PacketBridge bridge(absl::GetFlag(FLAGS_sut_address),
                      absl::GetFlag(FLAGS_control_address));
  ASSERT_THAT(bridge.Start(), IsOk());

  // Configure DVaaS params.
  dvaas::DataplaneValidationParams params;

  // Use pre-computed test vectors (skip automated synthesis).
  params.packet_test_vector_override = {BuildSimpleForwardingTestVector()};

  // Provide identity port map (SUT port X = control switch port X), skipping
  // the gNMI-based port mirroring that ValidateDataplane does by default.
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

  // Run upstream DVaaS validation!
  LOG(INFO) << "Running upstream DVaaS validation against 4ward...";
  auto result = validator.ValidateDataplane(testbed, params);

  bridge.Stop();
  LOG(INFO) << "PacketBridge forwarded " << bridge.PacketsForwarded()
            << " packets";

  ASSERT_THAT(result.status(), IsOk())
      << "DVaaS validation failed: " << result.status().message();
  LOG(INFO) << "DVaaS validation result: " << result->test_outcome_stats;
}

}  // namespace
}  // namespace fourward
