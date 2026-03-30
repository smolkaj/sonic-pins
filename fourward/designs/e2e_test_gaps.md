# E2E Test Gaps and Track Dependencies

**Status: mostly resolved** (updated 2026-03-30)

This document catalogs the gaps discovered by attempting to enable DISABLED
tests and push existing E2E tests further, along with the tracks that would
fix each gap.

## Resolved gaps

| Gap | Resolution | PR(s) |
|---|---|---|
| Auxiliary entries not auto-installed | Pre-packet hook + `CreateV1ModelAuxiliaryEntities` | 4ward#466, #469, #471; sonic-pins#52, #54 |
| No dataplane injection via FourwardPinsSwitch | `CreateDataplaneStub()` on FourwardPinsSwitch | sonic-pins#52 |
| FakeGnmiService Set is a no-op | Set now updates port state | sonic-pins#49 |
| PacketBridge routes by port number, not name | Routes by gNMI interface name | sonic-pins#48 |
| SynthesizePackets returns UNIMPLEMENTED | Implemented via p4-symbolic | sonic-pins#50 |
| `pins_auxiliary` role on VLAN disable tables | Annotated in P4 source | sonic-pins#47 |
| No BUILD targets for DISABLED tests | BUILD targets added | sonic-pins#52 |
| ACL trap requires PRE auxiliary | PRE clone session + ingress_clone_table entry | sonic-pins#52 |

### Enabled TDD tests (previously DISABLED)

1. **AclTrapPuntsPacketWithAuxEntries** — ACL trap punts via PRE clone session.
2. **AclTrapFailsWithoutAuxEntriesSucceedsWith** — causality proof (with vs without).
3. **L3ForwardingWorksWithAuxEntries** — forwarding works without manual VLAN disable.

## Open gaps

### 1. P4Runtime role-based read filtering

- **Test:** `AuxiliaryEntriesInvisibleToReads` (DISABLED)
- **What's needed:** 4ward's P4Runtime server must filter wildcard reads by
  role so `sdn_controller` doesn't see entries installed under the
  `pins_auxiliary` role.
- **Blocks DVaaS?** No (DVaaS doesn't wildcard-read entities), but matters
  for behavioral fidelity.

### 2. VLAN disable condition

- **Current behavior:** VLAN checks are unconditionally disabled.
- **What's needed:** Understand when real PINS switches set
  `SAI_DISABLE_VLAN_CHECKS` and make the hook conditional on the
  corresponding gNMI config.
- **Blocks DVaaS?** No (current behavior is correct for the common case).

### 3. Move static auxiliary entry builders to `sai_p4/tools/`

`MakeIngressCloneTableEntryForPunts()` and `MakeVlanDisableEntry()` are
SAI P4 concerns currently inlined in `fourward_pins_switch.cc`. They belong
in `sai_p4/tools/auxiliary_entries_for_v1model_targets.{h,cc}` alongside
`MakeV1modelPacketReplicationEngineEntryRequiredForPunts()`.

### 4. Additional E2E test vectors (quick wins)

- **Multi-route test vectors** — exercise multiple forwarding entries with
  different destination prefixes routed to different ports.
- **DROP test vector** — packet with no matching route should produce no
  output. Tests the DROP prediction path in `FourwardOracle`.
- **ACL deny test vector** — deny entry drops specific traffic. No PRE
  needed (deny = drop).

### 5. Port test gaps

- **Port test compile error** — `fourward_pins_switch_port_test.cc` calls
  `WaitForEnabledEthernetInterfacesToBeUp` which lives in a different header
  than included. Fix the include.
- **GnmiReportedP4rtIdsWorkInP4Runtime** — skeleton only, needs end-to-end
  port mapping + dataplane injection.
- **GnmiSetThenP4RuntimeWorks** — skeleton only, needs working gNMI Set +
  port remapping (gNMI Set now works, but test body is still TODO).
