#include "fourward/fourward_pins_switch.h"

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "dataplane.grpc.pb.h"
#include "dataplane.pb.h"
#include "fourward/fake_gnmi_service.h"
#include "fourward/fourward_server.h"
#include "github.com/openconfig/gnmi/proto/gnmi/gnmi.grpc.pb.h"
#include "google/protobuf/util/message_differencer.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "gutil/status.h"
#include "p4/v1/p4runtime.grpc.pb.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_pdpi/ir.h"
#include "p4_pdpi/ir.pb.h"
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

bool ContainsEntity(const std::vector<p4::v1::Entity>& entities,
                    const p4::v1::Entity& entity) {
  for (const auto& candidate : entities) {
    if (google::protobuf::util::MessageDifferencer::Equals(candidate, entity)) {
      return true;
    }
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

// Builds a VLAN disable table entry. The disable_vlan_checks_table uses a
// ternary dummy_match (wildcard, needs priority); the ingress/egress variants
// use LPM with prefix_length=0.
p4::v1::Entity MakeVlanDisableEntry(uint32_t table_id, uint32_t action_id,
                                    bool ternary = false) {
  p4::v1::Entity entity;
  auto* entry = entity.mutable_table_entry();
  entry->set_table_id(table_id);
  auto* match = entry->add_match();
  match->set_field_id(1);
  if (ternary) {
    match->mutable_ternary()->set_value(std::string(1, '\x00'));
    match->mutable_ternary()->set_mask(std::string(1, '\x00'));
    entry->set_priority(1);
  } else {
    match->mutable_lpm()->set_value(std::string(1, '\x00'));
    match->mutable_lpm()->set_prefix_len(0);
  }
  entry->mutable_action()->mutable_action()->set_action_id(action_id);
  return entity;
}

// Derives all auxiliary PI entities from the current state: entity-dependent
// entries (VLAN membership, L3 admit, loopback) via CreateV1ModelAuxiliaryEntities,
// plus static entries (PRE clone session, ingress_clone_table).
absl::StatusOr<std::vector<p4::v1::Entity>> DeriveAuxiliaryEntities(
    const fourward::dataplane::PrePacketHookInvocation& invocation,
    gnmi::gNMI::StubInterface& gnmi_stub) {
  std::vector<p4::v1::Entity> desired;

  // Static entries that don't depend on installed entities.
  desired.push_back(
      sai::MakeV1modelPacketReplicationEngineEntryRequiredForPunts());
  desired.push_back(MakeIngressCloneTableEntryForPunts());

  // VLAN disable entries. On real PINS switches, VLAN checks are disabled by the
  // platform based on gNMI config (SAI_DISABLE_VLAN_CHECKS). We unconditionally
  // disable them for now; the correct condition is not yet understood.
  // TODO: Make conditional on gNMI config once the triggering signal is known.
  desired.push_back(
      MakeVlanDisableEntry(DISABLE_VLAN_CHECKS_TABLE_ID,
                           DISABLE_VLAN_CHECKS_ACTION_ID, /*ternary=*/true));
  desired.push_back(
      MakeVlanDisableEntry(DISABLE_INGRESS_VLAN_CHECKS_TABLE_ID,
                           DISABLE_INGRESS_VLAN_CHECKS_ACTION_ID));
  desired.push_back(
      MakeVlanDisableEntry(DISABLE_EGRESS_VLAN_CHECKS_TABLE_ID,
                           DISABLE_EGRESS_VLAN_CHECKS_ACTION_ID));

  // Entity-dependent entries via CreateV1ModelAuxiliaryEntities.
  // Requires P4Info to convert PI entities to IR.
  if (invocation.has_p4info()) {
    ASSIGN_OR_RETURN(pdpi::IrP4Info ir_p4info,
                     pdpi::CreateIrP4Info(invocation.p4info()));

    // Convert PI entities from the invocation to IR.
    std::vector<p4::v1::Entity> pi_entities(invocation.entities().begin(),
                                            invocation.entities().end());
    ASSIGN_OR_RETURN(pdpi::IrEntities ir_entities,
                     pdpi::PiEntitiesToIr(ir_p4info, pi_entities));

    // Derive auxiliary entries from IR entities + gNMI state.
    ASSIGN_OR_RETURN(pdpi::IrEntities auxiliary_ir_entities,
                     sai::CreateV1ModelAuxiliaryEntities(
                         std::move(ir_entities), gnmi_stub));

    // Convert back to PI and add to desired.
    ASSIGN_OR_RETURN(std::vector<p4::v1::Entity> auxiliary_pi_entities,
                     pdpi::IrEntitiesToPi(ir_p4info, auxiliary_ir_entities));
    for (auto& entity : auxiliary_pi_entities) {
      desired.push_back(std::move(entity));
    }
  }

  return desired;
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
  // Owned by FourwardPinsSwitch; outlives HookState.
  FakeGnmiServer* gnmi_server;
};

// Runs on a background thread; exits when the stream closes (e.g. on cancel).
void FourwardPinsSwitch::RunHookLoop(HookState& hook) {
  std::unique_ptr<gnmi::gNMI::StubInterface> gnmi_stub;
  if (hook.gnmi_server != nullptr) {
    auto channel = grpc::CreateChannel(hook.gnmi_server->Address(),
                                       grpc::InsecureChannelCredentials());
    gnmi_stub = gnmi::gNMI::NewStub(channel);
  }

  fourward::dataplane::PrePacketHookInvocation invocation;
  while (hook.stream->Read(&invocation)) {
    absl::StatusOr<std::vector<p4::v1::Entity>> desired =
        gnmi_stub != nullptr
            ? DeriveAuxiliaryEntities(invocation, *gnmi_stub)
            : absl::StatusOr<std::vector<p4::v1::Entity>>(
                  std::vector<p4::v1::Entity>{});
    if (!desired.ok()) {
      LOG(ERROR) << "Failed to derive auxiliary entities: " << desired.status();
      // Respond with empty updates to unblock the server.
      hook.stream->Write(
          fourward::dataplane::PrePacketHookResponse::default_instance());
      continue;
    }

    std::vector<p4::v1::Update> updates =
        ComputeDelta(*desired, hook.installed_auxiliary_entities);
    hook.installed_auxiliary_entities = std::move(*desired);

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
    hook->gnmi_server = result.gnmi_server_.get();
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
