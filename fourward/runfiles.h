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

#ifndef PINS_FOURWARD_RUNFILES_H_
#define PINS_FOURWARD_RUNFILES_H_

#include <cstdlib>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace fourward {

// Resolves a workspace-relative path to an absolute Bazel runfile path.
// Falls back to the path as-is if not running under Bazel.
inline std::string BazelRunfile(absl::string_view path) {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  if (srcdir != nullptr && workspace != nullptr) {
    return absl::StrCat(srcdir, "/", workspace, "/", path);
  }
  return std::string(path);
}

}  // namespace fourward

#endif  // PINS_FOURWARD_RUNFILES_H_
