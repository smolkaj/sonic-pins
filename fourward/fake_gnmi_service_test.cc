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

#include "fourward/fake_gnmi_service.h"

#include <memory>
#include <string>
#include <vector>

#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.pb.h"
#include "gmock/gmock.h"
#include "grpcpp/channel.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gtest/gtest.h"
#include "gutil/status_matchers.h"

namespace dvaas {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

TEST(FakeGnmiServerTest, CreateSucceeds) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());
  EXPECT_THAT(server->Address(), HasSubstr("localhost:"));
}

TEST(FakeGnmiServerTest, GetStateReturnsInterfaces) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  grpc::ClientContext context;
  gnmi::GetRequest request;
  request.set_type(gnmi::GetRequest::STATE);
  gnmi::GetResponse response;
  grpc::Status status = stub->Get(&context, request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_GE(response.notification_size(), 1);
  ASSERT_GE(response.notification(0).update_size(), 1);
  std::string json = response.notification(0).update(0).val().json_ietf_val();

  // Default interfaces: Ethernet0..Ethernet7 with p4rt IDs 1..8.
  EXPECT_THAT(json, HasSubstr("Ethernet0"));
  EXPECT_THAT(json, HasSubstr("Ethernet7"));
  EXPECT_THAT(json, HasSubstr("oper-status"));
  EXPECT_THAT(json, HasSubstr("openconfig-p4rt:id"));
}

TEST(FakeGnmiServerTest, GetConfigReturnsInterfaces) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  grpc::ClientContext context;
  gnmi::GetRequest request;
  request.set_type(gnmi::GetRequest::CONFIG);
  gnmi::GetResponse response;
  grpc::Status status = stub->Get(&context, request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_GE(response.notification_size(), 1);
  std::string json = response.notification(0).update(0).val().json_ietf_val();
  EXPECT_THAT(json, HasSubstr("Ethernet0"));
  // Config uses "config" key, not "state".
  EXPECT_THAT(json, HasSubstr(R"("config":)"));
}

TEST(FakeGnmiServerTest, EmptySetRequestSucceeds) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  grpc::ClientContext context;
  gnmi::SetRequest request;
  gnmi::SetResponse response;
  grpc::Status status = stub->Set(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(FakeGnmiServerTest, CustomInterfaces) {
  std::vector<FakeInterface> interfaces = {
      {.name = "Ethernet42", .p4rt_id = 42},
  };
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create(interfaces));

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  grpc::ClientContext context;
  gnmi::GetRequest request;
  request.set_type(gnmi::GetRequest::STATE);
  gnmi::GetResponse response;
  grpc::Status status = stub->Get(&context, request, &response);

  ASSERT_TRUE(status.ok()) << status.error_message();
  std::string json = response.notification(0).update(0).val().json_ietf_val();
  EXPECT_THAT(json, HasSubstr("Ethernet42"));
  // Default interfaces should NOT be present.
  EXPECT_THAT(json, Not(HasSubstr("Ethernet0")));
}

// Helper to build a gNMI Set request that changes a port's P4RT ID.
gnmi::SetRequest BuildP4rtIdSetRequest(absl::string_view interface_name,
                                       int p4rt_id) {
  gnmi::SetRequest request;
  gnmi::Update* update = request.add_update();
  gnmi::Path* path = update->mutable_path();
  path->set_origin("openconfig");
  gnmi::PathElem* interfaces_elem = path->add_elem();
  interfaces_elem->set_name("interfaces");
  gnmi::PathElem* interface_elem = path->add_elem();
  interface_elem->set_name("interface");
  (*interface_elem->mutable_key())["name"] = std::string(interface_name);
  gnmi::PathElem* config_elem = path->add_elem();
  config_elem->set_name("config");
  gnmi::PathElem* id_elem = path->add_elem();
  id_elem->set_name("openconfig-p4rt:id");
  update->mutable_val()->set_json_ietf_val(
      absl::StrCat(R"({"openconfig-p4rt:id":)", p4rt_id, "}"));
  return request;
}

TEST(FakeGnmiServerTest, SetChangesP4rtIdAndGetReflectsIt) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  // Set Ethernet0's P4RT ID from 1 to 42.
  {
    grpc::ClientContext context;
    gnmi::SetRequest request = BuildP4rtIdSetRequest("Ethernet0", 42);
    gnmi::SetResponse response;
    grpc::Status status = stub->Set(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Verify CONFIG reflects the change.
  {
    grpc::ClientContext context;
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    gnmi::GetResponse response;
    grpc::Status status = stub->Get(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    std::string json = response.notification(0).update(0).val().json_ietf_val();
    // Ethernet0 should now have P4RT ID 42.
    EXPECT_THAT(json, HasSubstr(R"("openconfig-p4rt:id":42)"));
    // The old ID 1 for Ethernet0 should be gone. Ethernet1 still has ID 2.
    EXPECT_THAT(json, Not(HasSubstr(R"("openconfig-p4rt:id":1)")));
  }

  // Verify STATE also reflects the change.
  {
    grpc::ClientContext context;
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::STATE);
    gnmi::GetResponse response;
    grpc::Status status = stub->Get(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    std::string json = response.notification(0).update(0).val().json_ietf_val();
    EXPECT_THAT(json, HasSubstr(R"("openconfig-p4rt:id":42)"));
    // State should still report UP.
    EXPECT_THAT(json, HasSubstr(R"("oper-status":"UP")"));
  }
}

TEST(FakeGnmiServerTest, SetDeleteRemovesP4rtId) {
  std::vector<FakeInterface> interfaces = {
      {.name = "Ethernet0", .p4rt_id = 5},
  };
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create(interfaces));

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  // Delete Ethernet0's P4RT ID.
  {
    gnmi::SetRequest request;
    gnmi::Path* path = request.add_delete_();
    path->set_origin("openconfig");
    gnmi::PathElem* interfaces_elem = path->add_elem();
    interfaces_elem->set_name("interfaces");
    gnmi::PathElem* interface_elem = path->add_elem();
    interface_elem->set_name("interface");
    (*interface_elem->mutable_key())["name"] = "Ethernet0";
    gnmi::PathElem* config_elem = path->add_elem();
    config_elem->set_name("config");
    gnmi::PathElem* id_elem = path->add_elem();
    id_elem->set_name("openconfig-p4rt:id");

    grpc::ClientContext context;
    gnmi::SetResponse response;
    grpc::Status status = stub->Set(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Verify the P4RT ID is now 0 (unmapped).
  {
    grpc::ClientContext context;
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    gnmi::GetResponse response;
    grpc::Status status = stub->Get(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    std::string json = response.notification(0).update(0).val().json_ietf_val();
    EXPECT_THAT(json, HasSubstr(R"("openconfig-p4rt:id":0)"));
    EXPECT_THAT(json, Not(HasSubstr(R"("openconfig-p4rt:id":5)")));
  }
}

TEST(FakeGnmiServerTest, SetViaReplaceChangesP4rtId) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create());

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  // Use replace (not update) to change Ethernet0's P4RT ID.
  {
    gnmi::SetRequest request = BuildP4rtIdSetRequest("Ethernet0", 99);
    // Move the update to the replace field.
    *request.add_replace() = request.update(0);
    request.clear_update();

    grpc::ClientContext context;
    gnmi::SetResponse response;
    grpc::Status status = stub->Set(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Verify the change took effect.
  {
    grpc::ClientContext context;
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    gnmi::GetResponse response;
    grpc::Status status = stub->Get(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    std::string json = response.notification(0).update(0).val().json_ietf_val();
    EXPECT_THAT(json, HasSubstr(R"("openconfig-p4rt:id":99)"));
  }
}

TEST(FakeGnmiServerTest, SetForNonexistentInterfaceIsIgnored) {
  std::vector<FakeInterface> interfaces = {
      {.name = "Ethernet0", .p4rt_id = 1},
  };
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<FakeGnmiServer> server,
                        FakeGnmiServer::Create(interfaces));

  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      server->Address(), grpc::InsecureChannelCredentials());
  std::unique_ptr<gnmi::gNMI::Stub> stub = gnmi::gNMI::NewStub(channel);

  // Set on a nonexistent interface -- should succeed without effect.
  {
    grpc::ClientContext context;
    gnmi::SetRequest request = BuildP4rtIdSetRequest("EthernetBogus", 42);
    gnmi::SetResponse response;
    grpc::Status status = stub->Set(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Verify Ethernet0 is unchanged.
  {
    grpc::ClientContext context;
    gnmi::GetRequest request;
    request.set_type(gnmi::GetRequest::CONFIG);
    gnmi::GetResponse response;
    grpc::Status status = stub->Get(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    std::string json = response.notification(0).update(0).val().json_ietf_val();
    EXPECT_THAT(json, HasSubstr(R"("openconfig-p4rt:id":1)"));
    EXPECT_THAT(json, Not(HasSubstr("EthernetBogus")));
  }
}

}  // namespace
}  // namespace dvaas
