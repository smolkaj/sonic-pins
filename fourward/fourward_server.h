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

// Manages a 4ward P4Runtime server subprocess.
//
// Starts the server binary on a random OS-assigned port, waits for gRPC
// readiness by reading stdout for the "listening on port" banner, and kills the
// process on destruction.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(auto server, FourwardServer::Start({
//       .binary_path = "path/to/p4runtime_server.jar",
//       .device_id = 1,
//   }));
//   // server.Address() returns "localhost:<random-port>"
//   // server is killed when it goes out of scope.

#ifndef PINS_FOURWARD_FOURWARD_SERVER_H_
#define PINS_FOURWARD_FOURWARD_SERVER_H_

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "absl/time/time.h"

namespace fourward {

// RAII wrapper around a 4ward P4Runtime server child process.
class FourwardServer {
 public:
  struct Options {
    // Path to the server binary. If it ends in ".jar", the server is launched
    // via `java -jar <path>`.
    std::string binary_path;
    uint32_t device_id = 1;
    absl::Duration startup_timeout = absl::Seconds(60);
  };

  // Starts the server on a random port and blocks until it reports readiness.
  static absl::StatusOr<FourwardServer> Start(Options options);

  FourwardServer(const FourwardServer&) = delete;
  FourwardServer& operator=(const FourwardServer&) = delete;
  FourwardServer(FourwardServer&& other) noexcept;
  FourwardServer& operator=(FourwardServer&& other) noexcept;

  // Kills the server process (SIGTERM, then SIGKILL after 5s).
  ~FourwardServer();

  const std::string& Address() const { return address_; }
  int Port() const { return port_; }
  uint32_t DeviceId() const { return device_id_; }

 private:
  FourwardServer(pid_t pid, int port, uint32_t device_id);

  void Kill();

  pid_t pid_ = -1;
  int port_ = 0;
  uint32_t device_id_ = 0;
  std::string address_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SERVER_H_
