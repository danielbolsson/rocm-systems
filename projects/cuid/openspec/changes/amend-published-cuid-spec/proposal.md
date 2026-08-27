## Why

`openspec/specs/cuid/` now records the published specification as it stands at
version 84. Writing it down that way made three things visible that prose had
been hiding:

- **Bit 117 has two owners.** The Primary table gives UnitID part 2 six bits,
  112:117. The Derived table gives bit 117 to the Auxiliary Value Identifier. No
  producer can satisfy both, and the two implementations that exist picked
  differently.
- **The derived hash slot is described three ways.** The slot is 45 bits wide
  (72:116), labelled `bits [64:109]` which is 46, inside prose that says 110.
- **The auxiliary input structure is off by one in two adjacent fields.** Format
  gets 17 bits and Machine ID gets 127, so the ranges total 256 while neither
  field is the size the row's own description requires.

None of these is a drafting nicety. Each one produced a shipped defect, and each
was found by hand, months apart, by comparing code to code.

Alongside them the specification leaves values unstated that every producer
needs and therefore invents: the canonical fallback seed, the auxiliary fixed
key, the byte order of the PCIe Device Serial Number, and what a CPU puts in a
PCIe Routing ID field.

This change is the set of edits that would make the published page say what the
implementations must do. It carries no code. `add-cuid-kernel-interface`,
`pin-cuid-cross-layer-contract` and `integrate-cuid-into-amdsmi` already
implement all of it; this is the paperwork that has been trailing the code, and
which the kernel change's tasks 8.2, 8.3 and 8.4 have been blocked on.

## What Changes

- **Narrow UnitID part 2 to `112:116`**, five bits, and state that bit 117 is
  the Auxiliary Value Identifier in the Primary layout as well as the Derived
  one. **BREAKING** for any producer that packed a UnitID above 4095.
- **Fix the derived hash slot to `hash[64:108]`**, 45 bits, and correct the
  prose from 110 to 109. No conforming value changes; the table's own width was
  already right.
- **Repair the auxiliary input structure** to Format `0:15` and Machine ID
  `16:143`, so both fields are whole octets and the Machine ID can hold the
  128-bit `/etc/machine-id` the row names.
- **Correct "32bit MachineID" to 128 bits** in the fallback prose.
- **Withdraw UUIDv5 for auxiliary CUIDs.** An Auxiliary CUID becomes a normal
  UUIDv8 distinguished solely by bit 117, which the Derived table already
  defines for exactly that purpose. This voids the `amd.com` namespace string,
  the namespace form, and the question of HMAC operand order for the auxiliary
  path. **BREAKING** against the published text; not against either
  implementation, both of which already emit v8.
- **State the two key constants as literal octets** — `AMD-CUID-DEFAULT-SEED-v1`
  (24) and `AMD-CUID-TEMP-KEY-v1` (20), neither NUL-terminated nor padded.
- **State that the PCIe Device Serial Number is used in configuration-space
  order**, little-endian, unswapped, read from `dsn_cap_offset + 4`.
- **State that a CPU's auxiliary Routing ID field is zero**, since a CPU has no
  Bus/Device/Function of its own.
- **Renumber the device-type enumeration in S3** onto the on-wire values, and
  add NPU, which S3 does not list.
- **Retire S4** and repoint S3's fallback reference at S1.
- **Publish the conformance vectors** from
  `pin-cuid-cross-layer-contract/specs/cuid/conformance-vectors` as normative
  text in S1.

## Capabilities

### Modified Capabilities

- `cuid/primary-identifier`: UnitID part 2 narrows to `112:116`; bit 117 named
  in the Primary layout.
- `cuid/derived-identifier`: the hash slot pinned at `hash[64:108]`; the key
  constants stated; the prose corrected.
- `cuid/auxiliary-fallback`: uniform UUIDv8 replaces UUIDv5; the input structure
  repaired; the CPU Routing ID and the fixed key stated.
- `cuid/component-discovery`: the Device Serial Number's byte order stated; the
  NIC MAC fallback's orientation stated.

### New Capabilities

_None. Everything here is a correction to, or a filling-in of, something the
published page already covers._

## Impact

- **Specification pages**: S1 takes the field-table and prose corrections and
  the worked examples. S3 takes the enumeration renumbering. S4 is retired.
- **Implementations**: none. The kernel driver and the userspace library already
  do all of this; that is what makes these edits safe to make. The point of the
  change is that the page stops disagreeing with the code, so the next
  implementation does not have to guess.
- **Anything that recorded a library-emitted GPU CUID** before the enumeration
  renumbering holds a value that names a NIC. That is a consequence of the
  defect, not of this change, and is unavoidable either way.
