#include "fourward/trace_conversion.h"

#include <string>

#include "dvaas/packet_trace.pb.h"
#include "simulator.pb.h"

namespace fourward {
namespace {

// Recursively walks the TraceTree and appends DVaaS events to `trace`.
void FlattenTraceTree(const ::fourward::sim::TraceTree& tree,
                      dvaas::PacketTrace& trace) {
  for (const ::fourward::sim::TraceEvent& event : tree.events()) {
    if (event.has_table_lookup()) {
      const ::fourward::sim::TableLookupEvent& lookup = event.table_lookup();
      dvaas::Event& dvaas_event = *trace.add_events();
      dvaas::TableApply& table_apply = *dvaas_event.mutable_table_apply();
      table_apply.set_table_name(lookup.table_name());
      if (lookup.hit()) {
        table_apply.mutable_hit();
        // TODO: Convert PI TableEntry to PDPI IrTableEntry for
        // table_apply.hit().table_entry(). Requires PDPI on the sonic-pins
        // side, which has it — wire up when integrating into the frontend.
      } else {
        table_apply.mutable_miss();
      }
    } else if (event.has_mark_to_drop()) {
      dvaas::Event& dvaas_event = *trace.add_events();
      dvaas::MarkToDrop& mark_to_drop = *dvaas_event.mutable_mark_to_drop();
      if (event.has_source_info()) {
        mark_to_drop.set_source_location(
            event.source_info().source_fragment());
      }
    } else if (event.has_clone()) {
      dvaas::Event& dvaas_event = *trace.add_events();
      dvaas::PacketReplication& replication =
          *dvaas_event.mutable_packet_replication();
      replication.set_number_of_packets_replicated(1);
    }
    // Other event types (parser transitions, action executions, branch events,
    // extern calls, etc.) have no DVaaS equivalent — they are 4ward-specific
    // detail that DVaaS doesn't consume.
  }

  // Handle the outcome.
  if (tree.has_fork_outcome()) {
    const ::fourward::sim::Fork& fork = tree.fork_outcome();
    if (fork.reason() == ::fourward::sim::CLONE ||
        fork.reason() == ::fourward::sim::MULTICAST) {
      // Parallel fork: emit a replication event and recurse into all branches.
      if (fork.reason() == ::fourward::sim::MULTICAST) {
        dvaas::Event& dvaas_event = *trace.add_events();
        dvaas::PacketReplication& replication =
            *dvaas_event.mutable_packet_replication();
        replication.set_number_of_packets_replicated(fork.branches_size());
      }
      for (const ::fourward::sim::ForkBranch& branch : fork.branches()) {
        FlattenTraceTree(branch.subtree(), trace);
      }
    } else {
      // Alternative fork (action selector): follow the first branch.
      if (!fork.branches().empty()) {
        FlattenTraceTree(fork.branches(0).subtree(), trace);
      }
    }
  } else if (tree.has_packet_outcome()) {
    const ::fourward::sim::PacketOutcome& outcome = tree.packet_outcome();
    if (outcome.has_drop()) {
      dvaas::Event& dvaas_event = *trace.add_events();
      dvaas_event.mutable_drop();
    } else if (outcome.has_output()) {
      dvaas::Event& dvaas_event = *trace.add_events();
      dvaas::Transmit& transmit = *dvaas_event.mutable_transmit();
      transmit.set_port(
          std::string(outcome.output().p4rt_egress_port()));
      transmit.set_packet_size(outcome.output().payload().size());
    }
  }
}

}  // namespace

dvaas::PacketTrace TraceTreeToPacketTrace(
    const ::fourward::sim::TraceTree& trace_tree) {
  dvaas::PacketTrace trace;
  FlattenTraceTree(trace_tree, trace);
  return trace;
}

}  // namespace fourward
