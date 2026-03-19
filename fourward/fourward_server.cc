#include "fourward/fourward_server.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fourward {
namespace {

// The 4ward P4RuntimeServer prints this banner when ready.
constexpr absl::string_view kReadyBanner = "listening on port ";

// Parses the port number from the server's ready banner.
// The banner format is: "... listening on port <port>"
absl::StatusOr<int> ParsePortFromBanner(absl::string_view line) {
  auto pos = line.find(kReadyBanner);
  if (pos == absl::string_view::npos) {
    return absl::NotFoundError(
        absl::StrCat("Ready banner not found in: ", line));
  }
  absl::string_view port_str = line.substr(pos + kReadyBanner.size());
  int port;
  if (!absl::SimpleAtoi(port_str, &port)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse port from: ", port_str));
  }
  return port;
}

// Reads lines from `fd` until the ready banner is found or timeout expires.
// Returns the port number from the banner.
absl::StatusOr<int> WaitForReadyBanner(int fd, absl::Duration timeout) {
  absl::Time deadline = absl::Now() + timeout;
  std::string buffer;

  // Set non-blocking so we can poll with a timeout.
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  while (absl::Now() < deadline) {
    char chunk[1024];
    ssize_t n = read(fd, chunk, sizeof(chunk) - 1);
    if (n > 0) {
      chunk[n] = '\0';
      buffer.append(chunk, n);
      // Check each line for the ready banner.
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        auto port = ParsePortFromBanner(line);
        if (port.ok()) return *port;
      }
      // Check the remaining buffer (banner might not end with newline).
      auto port = ParsePortFromBanner(buffer);
      if (port.ok()) return *port;
    } else if (n == 0) {
      return absl::InternalError("Server process closed stdout unexpectedly");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return absl::InternalError(
          absl::StrCat("Read error: ", std::strerror(errno)));
    }
    absl::SleepFor(absl::Milliseconds(50));
  }
  return absl::DeadlineExceededError(absl::StrFormat(
      "4ward server did not produce ready banner within %s", timeout));
}

// Clears Bazel-specific environment variables that could interfere with the
// child process.
void ClearBazelEnvironment() {
  unsetenv("BUILD_WORKSPACE_DIRECTORY");
  unsetenv("BUILD_WORKING_DIRECTORY");
}

}  // namespace

FourwardServer::FourwardServer(pid_t pid, int port, uint64_t device_id)
    : pid_(pid),
      port_(port),
      device_id_(device_id),
      address_(absl::StrCat("localhost:", port)) {}

FourwardServer::FourwardServer(FourwardServer&& other) noexcept
    : pid_(other.pid_),
      port_(other.port_),
      device_id_(other.device_id_),
      address_(std::move(other.address_)) {
  other.pid_ = -1;
}

FourwardServer& FourwardServer::operator=(FourwardServer&& other) noexcept {
  if (this != &other) {
    Kill();
    pid_ = other.pid_;
    port_ = other.port_;
    device_id_ = other.device_id_;
    address_ = std::move(other.address_);
    other.pid_ = -1;
  }
  return *this;
}

FourwardServer::~FourwardServer() { Kill(); }

void FourwardServer::Kill() {
  if (pid_ <= 0) return;

  // Try graceful shutdown first.
  kill(pid_, SIGTERM);

  // Wait up to 5 seconds for the process to exit.
  absl::Time deadline = absl::Now() + absl::Seconds(5);
  while (absl::Now() < deadline) {
    int status;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      pid_ = -1;
      return;
    }
    absl::SleepFor(absl::Milliseconds(100));
  }

  // Force kill if still running.
  kill(pid_, SIGKILL);
  waitpid(pid_, nullptr, 0);
  pid_ = -1;
}

absl::StatusOr<FourwardServer> FourwardServer::Start(
    const std::string& binary_path, uint64_t device_id,
    absl::Duration startup_timeout) {
  // Create a pipe for the child's stdout.
  int stdout_pipe[2];
  if (pipe(stdout_pipe) != 0) {
    return absl::InternalError(
        absl::StrCat("pipe() failed: ", std::strerror(errno)));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return absl::InternalError(
        absl::StrCat("fork() failed: ", std::strerror(errno)));
  }

  if (pid == 0) {
    // Child process.
    close(stdout_pipe[0]);  // Close read end.
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stdout_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);

    ClearBazelEnvironment();

    std::string port_flag = "--port=0";
    std::string device_id_flag =
        absl::StrCat("--device-id=", device_id);

    std::vector<const char*> argv = {
        binary_path.c_str(),
        port_flag.c_str(),
        device_id_flag.c_str(),
        nullptr,
    };

    execv(binary_path.c_str(), const_cast<char* const*>(argv.data()));
    // If execv returns, it failed.
    _exit(127);
  }

  // Parent process.
  close(stdout_pipe[1]);  // Close write end.

  auto port = WaitForReadyBanner(stdout_pipe[0], startup_timeout);
  close(stdout_pipe[0]);

  if (!port.ok()) {
    // Clean up the child process.
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return port.status();
  }

  return FourwardServer(pid, *port, device_id);
}

}  // namespace fourward
