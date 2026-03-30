// Shared test utilities for fourward integration tests.

#ifndef PINS_FOURWARD_TEST_UTIL_H_
#define PINS_FOURWARD_TEST_UTIL_H_

#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "gutil/io.h"
#include "gutil/testing.h"
#include "p4/v1/p4runtime.pb.h"
#include "packetlib/packetlib.h"
#include "packetlib/packetlib.pb.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace dvaas {

// Loads the SAI middleblock ForwardingPipelineConfig compiled by 4ward
// (from the sai_middleblock_fourward.binpb runfile).
inline p4::v1::ForwardingPipelineConfig LoadSaiMiddleblock4wardConfig() {
  std::string error;
  std::unique_ptr<bazel::tools::cpp::runfiles::Runfiles> runfiles(
      bazel::tools::cpp::runfiles::Runfiles::CreateForTest(&error));
  CHECK(runfiles != nullptr) << "Failed to create Runfiles: " << error;
  absl::StatusOr<std::string> contents = gutil::ReadFile(
      runfiles->Rlocation("_main/fourward/sai_middleblock_fourward.binpb"));
  CHECK_OK(contents);
  p4::v1::ForwardingPipelineConfig config;
  CHECK(config.ParseFromString(*contents))
      << "Failed to parse sai_middleblock_fourward.binpb";
  return config;
}

// Parses a packetlib textproto, pads to minimum size, updates computed fields,
// and serializes to raw bytes.
inline std::string SerializeTestPacket(absl::string_view textproto) {
  packetlib::Packet packet =
      gutil::ParseProtoOrDie<packetlib::Packet>(textproto);
  CHECK_OK(packetlib::PadPacketToMinimumSize(packet));
  CHECK_OK(packetlib::UpdateAllComputedFields(packet));
  absl::StatusOr<std::string> serialized = packetlib::SerializePacket(packet);
  CHECK_OK(serialized);
  return *serialized;
}

}  // namespace dvaas

#endif  // PINS_FOURWARD_TEST_UTIL_H_
