#include "fourward/fourward_pins_switch.h"

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "google/protobuf/util/message_differencer.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_server.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/status.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "sai_p4/fixed/ids.h"
#include "sai_p4/tools/auxiliary_entries_for_v1model_targets.h"

namespace dvaas {
namespace {

// Returns an ingress_clone_table entry for punt-only cloning:
// marked_to_copy=1, marked_to_mirror=0 → ingress_clone(COPY_TO_CPU_SESSION_ID).
// This entry bridges the ACL trap action (which sets marked_to_copy) to the
// actual v1model clone() extern. On real PINS switches this logic is built in;
// on v1model targets it must be explicitly programmed.
p4::v1::Entity MakeIngressCloneTableEntryForPunts() {
  p4::v1::Entity entity;
  p4::v1::TableEntry* entry = entity.mutable_table_entry();
  entry->set_table_id(INGRESS_CLONE_TABLE_ID);

  // marked_to_copy = 1 (exact match, 1 bit).
  auto* match_copy = entry->add_match();
  match_copy->set_field_id(1);
  match_copy->mutable_exact()->set_value(std::string(1, '\x01'));

  // marked_to_mirror = 0 (exact match, 1 bit).
  auto* match_mirror = entry->add_match();
  match_mirror->set_field_id(2);
  match_mirror->mutable_exact()->set_value(std::string(1, '\x00'));

  // Required because mirror_egress_port is an optional match field.
  entry->set_priority(1);

  // Action: ingress_clone(clone_session = COPY_TO_CPU_SESSION_ID).
  auto* action = entry->mutable_action()->mutable_action();
  action->set_action_id(CLONING_INGRESS_CLONE_ACTION_ID);
  auto* param = action->add_params();
  param->set_param_id(1);
  // clone_session is bit<32>; encode as 4-byte big-endian.
  uint32_t session_id = COPY_TO_CPU_SESSION_ID;
  char bytes[4] = {
      static_cast<char>((session_id >> 24) & 0xFF),
      static_cast<char>((session_id >> 16) & 0xFF),
      static_cast<char>((session_id >> 8) & 0xFF),
      static_cast<char>(session_id & 0xFF),
  };
  param->set_value(std::string(bytes, 4));

  return entity;
}

bool EntityEquals(const p4::v1::Entity& a, const p4::v1::Entity& b) {
  return google::protobuf::util::MessageDifferencer::Equals(a, b);
}

bool ContainsEntity(const std::vector<p4::v1::Entity>& entities,
                    const p4::v1::Entity& entity) {
  for (const auto& candidate : entities) {
    if (EntityEquals(candidate, entity)) return true;
  }
  return false;
}

// Computes the delta between desired and installed entities, producing
// INSERT updates for new entities and DELETE updates for stale ones.
std::vector<p4::v1::Update> ComputeDelta(
    const std::vector<p4::v1::Entity>& desired,
    const std::vector<p4::v1::Entity>& installed) {
  std::vector<p4::v1::Update> updates;
  for (const auto& entity : desired) {
    if (!ContainsEntity(installed, entity)) {
      p4::v1::Update& update = updates.emplace_back();
      update.set_type(p4::v1::Update::INSERT);
      *update.mutable_entity() = entity;
    }
  }
  for (const auto& entity : installed) {
    if (!ContainsEntity(desired, entity)) {
      p4::v1::Update& update = updates.emplace_back();
      update.set_type(p4::v1::Update::DELETE);
      *update.mutable_entity() = entity;
    }
  }
  return updates;
}

}  // namespace

// Non-movable hook state, hidden behind a unique_ptr in FourwardPinsSwitch.
struct FourwardPinsSwitch::HookState {
  std::unique_ptr<fourward::dataplane::Dataplane::Stub> stub;
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<
      fourward::dataplane::PrePacketHookResponse,
      fourward::dataplane::PrePacketHookInvocation>>
      stream;
  std::vector<p4::v1::Entity> installed_auxiliary_entities;
  std::thread thread;
};

// Runs on a background thread; exits when the stream closes (e.g. on cancel).
void FourwardPinsSwitch::RunHookLoop(HookState& hook) {
  fourward::dataplane::PrePacketHookInvocation invocation;
  while (hook.stream->Read(&invocation)) {
    std::vector<p4::v1::Entity> desired;
    // PRE clone session for punting packets to CPU.
    desired.push_back(
        sai::MakeV1modelPacketReplicationEngineEntryRequiredForPunts());
    // ingress_clone_table entry bridging ACL trap → clone() extern.
    desired.push_back(MakeIngressCloneTableEntryForPunts());

    // TODO: Convert PI entities from invocation.entities() to IR and call
    // sai::CreateV1ModelAuxiliaryEntities for the full derivation (VLAN
    // membership, L3 admit, loopback).

    std::vector<p4::v1::Update> updates =
        ComputeDelta(desired, hook.installed_auxiliary_entities);
    hook.installed_auxiliary_entities = std::move(desired);

    fourward::dataplane::PrePacketHookResponse response;
    for (auto& update : updates) {
      *response.add_updates() = std::move(update);
    }
    hook.stream->Write(response);
  }
}

FourwardPinsSwitch::FourwardPinsSwitch(
    FourwardServer server, std::unique_ptr<FakeGnmiServer> gnmi_server)
    : server_(std::move(server)), gnmi_server_(std::move(gnmi_server)) {}

FourwardPinsSwitch::~FourwardPinsSwitch() {
  if (hook_ != nullptr) {
    hook_->context.TryCancel();
    if (hook_->thread.joinable()) hook_->thread.join();
  }
}
FourwardPinsSwitch::FourwardPinsSwitch(FourwardPinsSwitch&&) = default;
FourwardPinsSwitch& FourwardPinsSwitch::operator=(FourwardPinsSwitch&&) =
    default;

absl::StatusOr<FourwardPinsSwitch> FourwardPinsSwitch::Create(
    FourwardPinsSwitchOptions options) {
  ASSIGN_OR_RETURN(FourwardServer server,
                   FourwardServer::Start(options.device_id));
  ASSIGN_OR_RETURN(std::unique_ptr<FakeGnmiServer> gnmi_server,
                   FakeGnmiServer::Create(std::move(options.interfaces)));
  FourwardPinsSwitch result(std::move(server), std::move(gnmi_server));
  result.channel_ = grpc::CreateChannel(result.server_.Address(),
                                        grpc::InsecureChannelCredentials());

  if (options.enable_auxiliary_entries) {
    // Register the pre-packet hook for auxiliary entry reconciliation.
    auto hook = std::make_unique<HookState>();
    hook->stub = fourward::dataplane::Dataplane::NewStub(result.channel_);
    hook->stream = hook->stub->RegisterPrePacketHook(&hook->context);
    if (hook->stream == nullptr) {
      return absl::InternalError(
          "Failed to open RegisterPrePacketHook stream");
    }
    hook->thread = std::thread(RunHookLoop, std::ref(*hook));
    result.hook_ = std::move(hook);
  }
  return result;
}

absl::StatusOr<std::unique_ptr<p4::v1::P4Runtime::StubInterface>>
FourwardPinsSwitch::CreateP4RuntimeStub() {
  return p4::v1::P4Runtime::NewStub(channel_);
}

absl::StatusOr<std::unique_ptr<gnmi::gNMI::StubInterface>>
FourwardPinsSwitch::CreateGnmiStub() {
  // gNMI runs on a separate in-process server, not the 4ward server.
  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      gnmi_server_->Address(), grpc::InsecureChannelCredentials());
  return gnmi::gNMI::NewStub(channel);
}

std::unique_ptr<fourward::dataplane::Dataplane::Stub>
FourwardPinsSwitch::CreateDataplaneStub() {
  return fourward::dataplane::Dataplane::NewStub(channel_);
}

}  // namespace dvaas
