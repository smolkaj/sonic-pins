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

#include "fourward/fourward_server.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fourward {
namespace {

// Reads lines from `fd` until one matches "listening on port <N>" or the
// deadline expires. Returns the detected port number.
absl::StatusOr<int> WaitForReadyBanner(int fd, absl::Time deadline) {
  // Set fd to non-blocking so we can poll with a timeout.
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  std::string buffer;
  // Position up to which we've already searched for the banner.
  size_t search_pos = 0;
  char chunk[256];
  while (absl::Now() < deadline) {
    ssize_t n = read(fd, chunk, sizeof(chunk) - 1);
    if (n > 0) {
      chunk[n] = '\0';
      buffer.append(chunk, n);
      // Search only the newly appended data (plus overlap for partial match).
      constexpr absl::string_view kBanner = "listening on port ";
      size_t start = search_pos > kBanner.size()
                         ? search_pos - kBanner.size()
                         : 0;
      absl::string_view view(buffer);
      auto pos = view.find(kBanner, start);
      if (pos != absl::string_view::npos) {
        absl::string_view after = view.substr(pos + kBanner.size());
        int port = 0;
        if (absl::SimpleAtoi(
                after.substr(0, after.find_first_not_of("0123456789")),
                &port)) {
          return port;
        }
      }
      search_pos = buffer.size();
    } else if (n == 0) {
      return absl::InternalError(
          absl::StrCat("Server exited before reporting readiness. Output:\n",
                        buffer));
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return absl::InternalError(
          absl::StrCat("read() failed: ", std::strerror(errno)));
    }
    absl::SleepFor(absl::Milliseconds(100));
  }
  return absl::DeadlineExceededError(absl::StrCat(
      "Server did not report readiness within timeout. Output:\n", buffer));
}

}  // namespace

FourwardServer::FourwardServer(pid_t pid, int port, uint32_t device_id)
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

  LOG(INFO) << "Stopping 4ward server (pid=" << pid_ << ", port=" << port_
            << ")";

  kill(pid_, SIGTERM);

  for (int i = 0; i < 50; ++i) {
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      pid_ = -1;
      return;
    }
    absl::SleepFor(absl::Milliseconds(100));
  }

  LOG(WARNING) << "4ward server (pid=" << pid_
               << ") did not exit after SIGTERM, sending SIGKILL";
  kill(pid_, SIGKILL);
  waitpid(pid_, nullptr, 0);
  pid_ = -1;
}

absl::StatusOr<FourwardServer> FourwardServer::Start(Options options) {
  // Create pipe for reading child's stdout.
  int pipefd[2];
  if (pipe(pipefd) < 0) {
    return absl::InternalError(
        absl::StrCat("pipe() failed: ", std::strerror(errno)));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return absl::InternalError(
        absl::StrCat("fork() failed: ", std::strerror(errno)));
  }

  if (pid == 0) {
    // Child process: redirect stdout to pipe, exec the server binary.
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    // Always use --port=0 to let the OS pick a free port. The actual port
    // is parsed from the server's ready banner, avoiding any TOCTOU race.
    std::string port_flag = "--port=0";
    std::string device_id_flag =
        absl::StrFormat("--device-id=%d", options.device_id);

    // Clear Bazel runfiles env vars so the child process finds its own
    // runfiles rather than inheriting the test sandbox's paths.
    unsetenv("RUNFILES_DIR");
    unsetenv("RUNFILES_MANIFEST_FILE");
    unsetenv("RUNFILES_MANIFEST_ONLY");
    unsetenv("JAVA_RUNFILES");
    unsetenv("TEST_SRCDIR");

    std::vector<const char*> argv;
    if (absl::EndsWith(options.binary_path, ".jar")) {
      argv = {"java", "-jar", options.binary_path.c_str(),
              port_flag.c_str(), device_id_flag.c_str(), nullptr};
    } else {
      argv = {options.binary_path.c_str(), port_flag.c_str(),
              device_id_flag.c_str(), nullptr};
    }

    execvp(argv[0], const_cast<char* const*>(argv.data()));
    _exit(127);
  }

  // Parent process: read from pipe until server is ready.
  close(pipefd[1]);

  absl::Time deadline = absl::Now() + options.startup_timeout;
  auto detected_port = WaitForReadyBanner(pipefd[0], deadline);
  close(pipefd[0]);

  if (!detected_port.ok()) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return detected_port.status();
  }

  int actual_port = *detected_port;
  LOG(INFO) << "4ward server started: pid=" << pid
            << " address=localhost:" << actual_port
            << " device_id=" << options.device_id;
  return FourwardServer(pid, actual_port, options.device_id);
}

}  // namespace fourward
