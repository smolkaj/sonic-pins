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

// Minimal fake gNMI service modeling configurable Ethernet interfaces with P4RT
// port IDs. DVaaS uses gNMI to discover switch ports and check that they are
// up. This fake serves just enough to satisfy those queries.
//
// Runs as an in-process gRPC server. FourwardPinsSwitch connects to its address.

#ifndef PINS_FOURWARD_FAKE_GNMI_SERVICE_H_
#define PINS_FOURWARD_FAKE_GNMI_SERVICE_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.pb.h"
#include "grpcpp/grpcpp.h"

namespace dvaas {

// A single modeled Ethernet interface.
struct FakeInterface {
  std::string name;
  int p4rt_id;
  bool enabled = true;
  std::string oper_status = "UP";
};

// Fake gNMI service that models a set of Ethernet interfaces.
// Set updates P4RT port IDs; Get returns the current state.
class FakeGnmiService final : public gnmi::gNMI::Service {
 public:
  explicit FakeGnmiService(std::vector<FakeInterface> interfaces)
      : interfaces_(std::move(interfaces)) {
    RebuildJsonLocked();
  }

  // Creates a default set of 8 Ethernet interfaces.
  static std::vector<FakeInterface> DefaultInterfaces() {
    std::vector<FakeInterface> result;
    for (int i = 0; i < 8; ++i) {
      result.push_back(
          {.name = absl::StrCat("Ethernet", i), .p4rt_id = i + 1});
    }
    return result;
  }

  grpc::Status Get(grpc::ServerContext* /*context*/,
                   const gnmi::GetRequest* request,
                   gnmi::GetResponse* response) override {
    absl::MutexLock lock(&mu_);
    gnmi::Notification* notification = response->add_notification();
    gnmi::Update* update = notification->add_update();
    update->mutable_val()->set_json_ietf_val(
        request->type() == gnmi::GetRequest::CONFIG ? config_json_
                                                    : state_json_);
    return grpc::Status::OK;
  }

  grpc::Status Set(grpc::ServerContext* /*context*/,
                   const gnmi::SetRequest* request,
                   gnmi::SetResponse* /*response*/) override {
    absl::MutexLock lock(&mu_);
    // Process updates and replaces -- both carry a path + value.
    for (const gnmi::Update& update : request->update()) {
      ApplyUpdateLocked(update);
    }
    for (const gnmi::Update& replace : request->replace()) {
      ApplyUpdateLocked(replace);
    }
    // Deletes of P4RT IDs set the port ID to 0 (unmapped).
    for (const gnmi::Path& path : request->delete_()) {
      std::string interface_name = ExtractInterfaceName(path);
      if (interface_name.empty()) continue;
      if (!IsP4rtIdPath(path)) continue;
      for (FakeInterface& iface : interfaces_) {
        if (iface.name == interface_name) {
          iface.p4rt_id = 0;
          break;
        }
      }
    }
    RebuildJsonLocked();
    return grpc::Status::OK;
  }

  grpc::Status Capabilities(
      grpc::ServerContext* /*context*/,
      const gnmi::CapabilityRequest* /*request*/,
      gnmi::CapabilityResponse* /*response*/) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "Capabilities not supported");
  }

  grpc::Status Subscribe(
      grpc::ServerContext* /*context*/,
      grpc::ServerReaderWriter<gnmi::SubscribeResponse,
                               gnmi::SubscribeRequest>* /*stream*/) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "Subscribe not supported");
  }

 private:
  // Extracts the interface name from a gNMI path like
  // interfaces/interface[name=Ethernet0]/config/openconfig-p4rt:id.
  static std::string ExtractInterfaceName(const gnmi::Path& path) {
    for (const gnmi::PathElem& elem : path.elem()) {
      if (elem.name() == "interface") {
        auto it = elem.key().find("name");
        if (it != elem.key().end()) return it->second;
      }
    }
    return "";
  }

  // Returns true if the path's leaf element is the P4RT ID.
  static bool IsP4rtIdPath(const gnmi::Path& path) {
    if (path.elem().empty()) return false;
    const std::string& leaf = path.elem().rbegin()->name();
    return leaf == "openconfig-p4rt:id" || leaf == "id";
  }

  // Extracts the P4RT ID from a json_ietf_val like {"openconfig-p4rt:id":5}.
  // Returns -1 if not found.
  static int ExtractP4rtIdFromJson(absl::string_view json) {
    // Minimal JSON parsing: find "openconfig-p4rt:id" followed by a colon and
    // integer. Avoids pulling in a JSON library for this single use case.
    static constexpr absl::string_view kKey = "\"openconfig-p4rt:id\":";
    auto pos = json.find(kKey);
    if (pos == absl::string_view::npos) return -1;
    absl::string_view rest = json.substr(pos + kKey.size());
    // Skip whitespace.
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
      rest.remove_prefix(1);
    }
    int value;
    // Find end of number.
    size_t end = 0;
    while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') ++end;
    if (end > 0 && absl::SimpleAtoi(rest.substr(0, end), &value)) {
      return value;
    }
    return -1;
  }

  void ApplyUpdateLocked(const gnmi::Update& update) {
    std::string interface_name = ExtractInterfaceName(update.path());
    if (interface_name.empty()) return;
    if (!IsP4rtIdPath(update.path())) return;
    int new_id = ExtractP4rtIdFromJson(update.val().json_ietf_val());
    if (new_id < 0) return;
    for (FakeInterface& iface : interfaces_) {
      if (iface.name == interface_name) {
        iface.p4rt_id = new_id;
        return;
      }
    }
  }

  void RebuildJsonLocked() {
    config_json_ = BuildInterfacesJson(interfaces_, /*config=*/true);
    state_json_ = BuildInterfacesJson(interfaces_, /*config=*/false);
  }

  static std::string BuildInterfacesJson(
      const std::vector<FakeInterface>& interfaces, bool config) {
    std::vector<std::string> entries;
    for (const FakeInterface& iface : interfaces) {
      std::string inner;
      if (config) {
        inner = absl::StrFormat(
            R"("config":{"name":"%s","type":"iana-if-type:ethernetCsmacd",)"
            R"("enabled":%s,"openconfig-p4rt:id":%d})",
            iface.name, iface.enabled ? "true" : "false", iface.p4rt_id);
      } else {
        inner = absl::StrFormat(
            R"("state":{"name":"%s","type":"iana-if-type:ethernetCsmacd",)"
            R"("enabled":%s,"oper-status":"%s","openconfig-p4rt:id":%d})",
            iface.name, iface.enabled ? "true" : "false", iface.oper_status,
            iface.p4rt_id);
      }
      entries.push_back(
          absl::StrFormat(R"({"name":"%s",%s})", iface.name, inner));
    }
    return absl::StrFormat(
        R"({"openconfig-interfaces:interfaces":{"interface":[%s]}})",
        absl::StrJoin(entries, ","));
  }

  absl::Mutex mu_;
  std::vector<FakeInterface> interfaces_ ABSL_GUARDED_BY(mu_);
  std::string config_json_ ABSL_GUARDED_BY(mu_);
  std::string state_json_ ABSL_GUARDED_BY(mu_);
};

// Starts a FakeGnmiService on a random port. The server runs until the
// returned object is destroyed.
class FakeGnmiServer {
 public:
  // Starts a fake gNMI server with the given interfaces.
  static absl::StatusOr<std::unique_ptr<FakeGnmiServer>> Create(
      std::vector<FakeInterface> interfaces =
          FakeGnmiService::DefaultInterfaces()) {
    // Can't use make_unique due to private constructor.
    std::unique_ptr<FakeGnmiServer> result(
        new FakeGnmiServer(std::move(interfaces)));
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&result->service_);
    result->server_ = builder.BuildAndStart();
    if (result->server_ == nullptr) {
      return absl::InternalError("Failed to start fake gNMI server");
    }
    result->address_ = absl::StrCat("localhost:", port);
    return result;
  }

  const std::string& Address() const { return address_; }
  FakeGnmiService& Service() { return service_; }

 private:
  explicit FakeGnmiServer(std::vector<FakeInterface> interfaces)
      : service_(std::move(interfaces)) {}

  FakeGnmiService service_;
  std::unique_ptr<grpc::Server> server_;
  std::string address_;
};

}  // namespace dvaas

#endif  // PINS_FOURWARD_FAKE_GNMI_SERVICE_H_
