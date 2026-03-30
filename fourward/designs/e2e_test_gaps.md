# E2E Test Gaps and Track Dependencies

**Status: findings**

This document catalogs the gaps discovered by attempting to enable DISABLED
tests and push existing E2E tests further, along with the tracks that would
fix each gap.

## Step 1: DISABLED tests in fourward_pins_switch_test.cc

These 4 tests verify the auxiliary entry contract: PRE clone sessions, VLAN
membership entries, and other P4 entries that a real PINS switch derives from
gNMI config but that must be explicitly installed on a P4-simulated switch.

**Build status:** No `cc_test` target exists in `fourward/BUILD.bazel` for
`fourward_pins_switch_test.cc`. The file compiles against existing headers
(all includes resolve), but needs a BUILD target with deps on
`:fourward_switch`, `//p4_infra/p4_runtime:p4_runtime_session`,
`//p4_infra/p4_runtime:p4_runtime_session_extras`,
`//sai_p4/instantiations/google/test_tools:test_entries`, `packetlib`, and
the `:sai_middleblock_fourward` data dependency.

### Test 1: AclTrapPuntsPacketWithAuxEntries

- **What it tests:** ACL trap entry punts packets to CPU via PRE clone
  session. Without the auxiliary PRE entry, the punt fails silently.
- **Status:** Would compile but cannot run. Two blockers:
  1. No mechanism to inject packets into the switch's dataplane (the test
     has a TODO for "Use switch's Dataplane service once exposed").
  2. Auxiliary entries (PRE clone sessions) are not automatically installed.
     The test installs only an ACL trap via `sai::EntryBuilder`, but the
     PRE clone session that makes trapping work is never created.
- **Fixing tracks:**
  - Track D (auxiliary entry reconciliation): Implement `FourwardPinsSwitch` pre-packet
    hook that calls `sai::CreateV1ModelAuxiliaryEntities` to install PRE
    entries transparently.
  - Dataplane exposure (new requirement): Expose the 4ward Dataplane gRPC service
    through `FourwardSwitch` so tests can inject packets directly.
- **Priority:** Blocks DVaaS on real switches (auxiliary entries are
  required for punting, which is required for DVaaS packet injection).

### Test 2: L3ForwardingWorksWithAuxEntries

- **What it tests:** L3 forwarding requires VLAN check disable entries.
  Without them, packets are dropped before reaching routing tables.
- **Status:** Same two blockers as Test 1 (no packet injection, no
  auxiliary entries).
- **Fixing tracks:**
  - Track D (auxiliary entry reconciliation): VLAN disable entries must be automatically
    derived from gNMI state.
  - Dataplane exposure (new requirement): Needed to verify packet forwarding.
- **Priority:** Blocks DVaaS. The existing E2E test
  (`ValidateDataplaneWithUserProvidedTestVectors`) works around this by
  manually calling `AddDisableVlanChecksEntry()`, but that's a test-only
  workaround. A real DVaaS flow needs transparent auxiliary entries.

### Test 3: AuxiliaryEntriesInvisibleToReads

- **What it tests:** Wildcard P4Runtime reads by `sdn_controller` role
  should NOT return auxiliary entries installed by `pins_auxiliary` role.
- **Status:** Blocked on:
  1. Auxiliary entry installation (Track C).
  2. P4Runtime role-based filtering: 4ward's P4Runtime server may not
     support role-scoped reads. Needs investigation.
- **Fixing tracks:**
  - Track D (auxiliary entry reconciliation): Must be installed under a separate role.
  - Track H (pins_auxiliary role): 4ward server needs role-based read filtering.
- **Priority:** Nice-to-have for correctness. DVaaS works without this
  (it doesn't wildcard-read entries), but it matters for behavioral
  fidelity.

### Test 4: AclTrapFailsWithoutAuxEntriesSucceedsWith

- **What it tests:** Causality proof. Same config, same packet: without
  auxiliary entries punt fails, with them it succeeds.
- **Status:** Blocked on all of Tests 1's blockers, plus the ability to
  toggle the pre-packet hook on/off within a single test.
- **Fixing tracks:**
  - Track D (auxiliary entry reconciliation): Core requirement.
  - Dataplane exposure (new requirement): Core requirement.
- **Priority:** Nice-to-have for test confidence. Not a DVaaS blocker
  itself, but validates the auxiliary entry mechanism.

## Step 1 (cont.): DISABLED tests in fourward_pins_switch_port_test.cc

These 5 tests verify the port identity contract from
`designs/port_identity.md`: gNMI interface names, P4RT port IDs, and
dataplane port numbers must be consistent.

**Build status:** No `cc_test` target in `fourward/BUILD.bazel`. The file
includes `lib/gnmi/gnmi_helper.h` but calls
`pins_test::WaitForEnabledEthernetInterfacesToBeUp` which lives in
`tests/lib/switch_test_setup_helpers.h` (different header). This is a
**compile error** that must be fixed before the test can build.

### Test 1: GnmiSetConverges

- **What it tests:** After remapping port IDs via gNMI Set, state path
  converges. DVaaS depends on this after mirroring SUT port config.
- **Status:** Compile error (wrong include). Even after fixing the include,
  `FakeGnmiService::Set` is a no-op stub (accepts but ignores all
  requests). gNMI Set doesn't actually change port state.
- **Fixing tracks:**
  - Track B (FakeGnmiService Set): `FakeGnmiService` needs mutable state that
    updates on Set requests.
- **Priority:** Blocks DVaaS when SUT has non-default port IDs (DVaaS
  mirrors SUT gNMI config to control switch, which requires Set to work).

### Test 2: GnmiReportedP4rtIdsWorkInP4Runtime

- **What it tests:** Read P4RT ID from gNMI, use it in a table entry,
  verify the packet uses the correct port.
- **Status:** Skeleton only (all TODO). Requires:
  1. gNMI → P4RT ID mapping to be implemented.
  2. Dataplane injection to verify packet egress port.
- **Fixing tracks:**
  - Track B (FakeGnmiService Set): End-to-end port mapping.
  - Dataplane exposure (new requirement).
- **Priority:** Blocks DVaaS correctness (wrong port mapping = wrong
  test results).

### Test 3: BridgeRoutesByInterfaceName

- **What it tests:** Two switches with different P4RT IDs for the same
  interface name. PacketBridge routes by interface name, not port number.
- **Status:** Skeleton only. `FourwardSwitch::Create` accepts custom
  interfaces, but `PacketBridge` may not resolve interface names.
- **Fixing tracks:**
  - Track B (FakeGnmiService Set): Interface-name-based bridge routing.
  - Track G (PacketBridge): Bridge needs interface name resolution.
- **Priority:** Blocks DVaaS when SUT and control switch have different
  port numbering (which is the normal case with real SUTs).

### Test 4: GnmiSetThenP4RuntimeWorks

- **What it tests:** Remap ports via gNMI Set, then install entries using
  new P4RT IDs. This is the actual DVaaS flow.
- **Status:** Skeleton only. Requires working gNMI Set + port remapping.
- **Fixing tracks:**
  - Track B (FakeGnmiService Set): Full flow.
- **Priority:** Blocks DVaaS on switches with non-default port mapping.

### Test 5: UnmappedPortDropsPacket

- **What it tests:** Forward to a P4RT port ID with no gNMI interface.
  Packet should be dropped (with a warning).
- **Status:** Skeleton only. Requires port validation layer.
- **Fixing tracks:**
  - Track B (FakeGnmiService Set): Port validation.
- **Priority:** Nice-to-have. Improves error reporting but doesn't block
  DVaaS.

## Step 2: Pushing the E2E test further

### ValidateDataplaneWithUserProvidedTestVectors (PASSING)

This is the "north star" test in `portable_pins_backend_test.cc`. It
currently passes by:
- Manually installing VLAN disable entries (`AddDisableVlanChecksEntry`,
  `AddDisableIngressVlanChecksEntry`, `AddDisableEgressVlanChecksEntry`).
- Providing a single hand-crafted test vector (one IPv4/UDP packet
  forwarded to port "1").
- Using an identity port map (port N on SUT = port N on control switch).

#### Gap: Multiple forwarding entries

Adding multiple routes (e.g. different destination prefixes to different
ports) would exercise the test vector generation and prediction paths more
thoroughly. The existing `GeneratePacketTestVectors` method handles this,
but the E2E test only uses one route. Extending this is straightforward
and doesn't require new infrastructure.

**Fixing tracks:** None (just test expansion). Can be done immediately.

#### Gap: Packets that should be dropped

No test vector exercises the "packet dropped" path. A packet with a
destination that doesn't match any route should produce no output. The
4ward oracle should predict DROP, and the SUT should also drop it. This
tests the DROP prediction path in `FourwardOracle`.

**Fixing tracks:** None (just test expansion). Can be done immediately.

#### Gap: ACL entries

The passing E2E test only uses forwarding entries. Adding ACL entries
(e.g. deny entries that drop specific traffic, or copy-to-CPU entries)
would exercise the ACL pipeline. However, punt/trap ACL entries require
PRE auxiliary entries (Track C) to work.

**Fixing tracks:**
- ACL deny: Can be done immediately (deny = drop, no PRE needed).
- ACL trap/copy: Requires Track C (auxiliary entries for PRE clone session).

**Priority:** ACL deny is low-hanging fruit. ACL trap is blocked.

### DISABLED_ValidateDataplaneWithSynthesizedTestVectors

This test (line 361 of `portable_pins_backend_test.cc`) is the ultimate
goal: DVaaS synthesizes test packets automatically via p4-symbolic. It's
DISABLED because `SynthesizePackets` returns `UNIMPLEMENTED` in the
portable PINS backend.

**Fixing tracks:**
- Track E (p4-symbolic synthesis): Implement `SynthesizePackets` in the
  portable PINS backend, likely by integrating with p4-symbolic or by
  using 4ward's own packet generation capabilities.

**Priority:** Blocks fully automated DVaaS (no manual test vectors).
Currently the highest-impact gap for production use.

## Step 3: Gap Summary

| Gap | Test(s) | Fixing Track | Blocks DVaaS? |
|---|---|---|---|
| No BUILD targets for DISABLED tests | All 9 DISABLED tests | Infra | No (tests are TDD specs) |
| Port test compile error (wrong include) | Port tests 1-5 | Bug fix | No |
| Auxiliary entries not auto-installed | Switch tests 1-4 | Track C | **Yes** (punting) |
| No dataplane injection via FourwardSwitch | Switch tests 1-2,4; Port test 2 | Track D | **Yes** (packet I/O) |
| P4Runtime role-based read filtering | Switch test 3 | Track E | No |
| FakeGnmiService Set is a no-op | Port tests 1,4 | Track F | **Yes** (port remapping) |
| PacketBridge routes by port number, not name | Port test 3 | Track G | **Yes** (heterogeneous testbed) |
| SynthesizePackets returns UNIMPLEMENTED | Synthesized test vectors | Track H | **Yes** (auto test gen) |
| No multi-route E2E test | (new test needed) | None | No |
| No DROP E2E test | (new test needed) | None | No |
| No ACL deny E2E test | (new test needed) | None | No |
| ACL trap requires PRE auxiliary | (new test needed) | Track C | **Yes** |

### Priority ordering for DVaaS

1. **Track D (auxiliary entry reconciliation)** and **Dataplane exposure (new requirement)** are
   the most critical. Without them, the simulated switch can't punt
   packets or inject test traffic, which are fundamental to DVaaS.

2. **Track B (FakeGnmiService Set)** is next. DVaaS mirrors SUT port config to
   the control switch, which requires gNMI Set to actually work.

3. **Track G (PacketBridge interface routing)** is needed when the SUT and
   control switch have different port numbering (the normal case).

4. **Track E (p4-symbolic synthesis)** is needed for fully automated DVaaS
   (no manual test vectors).

5. **Track H (pins_auxiliary role)** is nice-to-have for behavioral fidelity.

### Quick wins (no track dependency)

- Add multi-route test vectors to the passing E2E test.
- Add a DROP test vector (packet with no matching route).
- Add an ACL deny test vector.
- Fix the compile error in `fourward_pins_switch_port_test.cc` (wrong
  include for `WaitForEnabledEthernetInterfacesToBeUp`).
- Add BUILD targets for the DISABLED test files (they serve as executable
  specs even while DISABLED).
