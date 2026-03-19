// RAII wrapper for a 4ward P4Runtime server subprocess.
//
// Starts a 4ward P4RuntimeServer as a child process, waits for it to become
// ready, and terminates it on destruction. The server listens on an
// OS-assigned port; the actual port is parsed from the server's startup banner.
//
// Usage:
//   ASSERT_OK_AND_ASSIGN(FourwardServer server,
//                         FourwardServer::Start(binary_path));
//   // server.Address() == "localhost:<port>"
//   // ... use gRPC to talk to the server ...
//   // Server is killed when `server` goes out of scope.

#ifndef PINS_FOURWARD_FOURWARD_SERVER_H_
#define PINS_FOURWARD_FOURWARD_SERVER_H_

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "absl/time/time.h"

namespace fourward {

class FourwardServer {
 public:
  // Starts a 4ward P4RuntimeServer subprocess. `binary_path` is the path to
  // the server binary (a kt_jvm_binary or java_binary runfile). The server
  // is started with `--port=0` to bind to an ephemeral port; the actual port
  // is detected from the server's startup banner on stdout.
  //
  // `device_id` is passed to the server via `--device-id`.
  // `startup_timeout` controls how long to wait for the ready banner.
  static absl::StatusOr<FourwardServer> Start(
      const std::string& binary_path, uint64_t device_id = 1,
      absl::Duration startup_timeout = absl::Seconds(60));

  // Movable but not copyable.
  FourwardServer(FourwardServer&& other) noexcept;
  FourwardServer& operator=(FourwardServer&& other) noexcept;
  FourwardServer(const FourwardServer&) = delete;
  FourwardServer& operator=(const FourwardServer&) = delete;

  // Kills the server subprocess (SIGTERM, then SIGKILL after 5s).
  ~FourwardServer();

  // Returns "localhost:<port>" — the address to connect gRPC clients to.
  const std::string& Address() const { return address_; }

  // Returns the port the server is listening on.
  int Port() const { return port_; }

  // Returns the device ID the server was started with.
  uint64_t DeviceId() const { return device_id_; }

 private:
  FourwardServer(pid_t pid, int port, uint64_t device_id);
  void Kill();

  pid_t pid_ = -1;
  int port_ = 0;
  uint64_t device_id_ = 0;
  std::string address_;
};

}  // namespace fourward

#endif  // PINS_FOURWARD_FOURWARD_SERVER_H_
