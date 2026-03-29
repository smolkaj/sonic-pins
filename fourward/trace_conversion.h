// Converts 4ward's TraceTree to DVaaS's flat PacketTrace.
//
// 4ward produces a recursive TraceTree with forks for non-deterministic choice
// points (action selectors, clone, multicast). DVaaS consumes a flat list of
// events (PacketTrace). This library flattens the tree by walking the first
// branch at each fork, collecting events along the way.

#ifndef PINS_FOURWARD_TRACE_CONVERSION_H_
#define PINS_FOURWARD_TRACE_CONVERSION_H_

#include "dvaas/packet_trace.pb.h"
#include "simulator.pb.h"

namespace fourward {

// Flattens a 4ward TraceTree into a DVaaS PacketTrace.
//
// At non-deterministic fork points (action selectors), follows the first
// branch. At parallel forks (clone, multicast), follows the original branch
// and emits a PacketReplication event.
dvaas::PacketTrace TraceTreeToPacketTrace(
    const ::fourward::sim::TraceTree& trace_tree);

}  // namespace fourward

#endif  // PINS_FOURWARD_TRACE_CONVERSION_H_
