## Context

See `proposal.md` for motivation. The design-relevant state:

- Two independent producers exist. The kernel driver (`amdgpu_cuid.c`, on
  `dgalants/cuid-refactor`) and the userspace library (`projects/cuid` in
  `rocm-systems`) each compute the whole format from scratch. Nothing forces
  them to agree, and they have been verified to agree exactly once, by hand, on
  two W6800s.
- The kernel side is already conforming for the parts this change touches: the
  packing, the framing including payload bits 120:121, the 45-bit derived fold
  and the canonical seed value. The library is not.
- Four specification pages disagree with each other. S1 is normative and its
  derived table is off by one against its own prose; S3 mandates bit 117 and
  publishes a device-type enumeration offset by one from S1's; S4 is empty and
  is the page S1's own reply cites as the authority for the bit tables.
- Two answers were requested and did not arrive: the canonical seed's exact
  bytes and the temporary key's. Both are placeholders in both layers today, and
  both are byte-identical across the two layers today.

The constraint that shapes everything below: **no conforming value may change
unless it is currently wrong.** Every emitted CUID that is correct under the
uniform-v8 reading must survive this change untouched, because the alternative
is invalidating identifiers already recorded in the field.

## Goals / Non-Goals

**Goals:**

- Turn each remaining open question into a decision with a stated reason and a
  stated reversal cost, so it cannot be re-opened by silence.
- Express the format as values, not prose, and make those values executable.
- Make a future divergence between the two layers a build failure rather than a
  discovery.

**Non-Goals:**

- Changing anything the kernel already does correctly. The only kernel-side
  requirement here is the seed length check.
- The kernel's auxiliary path. It is deleted by `add-cuid-kernel-interface`, not
  redefined here.
- Seed persistence across module reload, node-wide seed plumbing, the IOCTL, and
  the hypervisor opt-out. All tracked elsewhere; none of them changes a value.
- Rewriting the specification pages. This change states what they should say;
  someone with edit rights has to make the edits.

## Decisions

### D1 — Uniform UUIDv8 with bit 117, not UUIDv5 for the auxiliary

Both packages were acceptable to the specification's author, and this one is
what the kernel and S3 already implement.

*Alternative considered:* auxiliary CUIDs typed as UUIDv5, distinguished by the
version nibble, with the derived slot widening to 46 bits.

*Rejected because* it is an ABI break with nothing on the other side of it. A
v5-typed value carrying an HMAC-SHA-256 payload is not a conforming v5 UUID —
RFC 9562 defines v5 as SHA-1 over `namespace ‖ name` — so a validating parser
may reject it and some libraries re-canonicalise on it. It would split every
consumer onto two parse paths. It would make the same device present different
UUID versions depending on the privilege of the caller, which advertises a
property of the environment as a property of the device. And it invalidates
every derived CUID emitted so far, plus requires an S3 revision.

*Reversal cost:* v8 is the reversible direction. Choosing v8 changes no emitted
value; choosing v5 changes all of them. If v5 is later insisted on, nothing here
has been foreclosed except the effort.

*Consequence, stated so it is not re-litigated:* the auxiliary marker is bit 117;
the derived slot stays 45 bits; S1's derived slice renumbers to `hash[64:108]`
and its prose drops 110 → 109; S1's primary table becomes `112:116`; S3 stands
unchanged; the namespace string, namespace form and HMAC operand-order questions
cease to exist; and the temporary fixed key becomes necessary after all, because
without a namespace nothing else separates the two derivations.

The reference to UUIDv5 may remain in S1 as a descriptive note on the
*derivation style*. It must not describe the version nibble. Those two readings
of "v5" are what kept the question alive.

### D2 — Constants are ASCII, unpadded

Both keys are fixed at the bytes the two layers already ship, with no NUL and no
padding.

*Alternative considered:* padding both to 32 octets, as the kernel currently does
for the temporary key.

*Rejected because* padding to 32 is not an HMAC operation. HMAC-SHA-256 pads a
short key to its own **64**-octet block size internally; zero-padding to 32
first produces a materially different key and therefore a different digest, for
no cryptographic gain. It also creates a second, silent way to get the constant
wrong. Neither key is a secret — one is a documented public placeholder, the
other must be reproducible by unprivileged software by definition — so their
length carries no security argument at all.

*Reversal cost:* zero for the canonical seed, which is already unpadded in both
layers. Non-zero for the temporary key, which the kernel pads today — but the
kernel's copy is being deleted, so only the library moves, and the library
currently uses an entirely different construction anyway.

### D3 — One derivation function, one operand order

Every derivation is `HMAC-SHA256(key, message = the 16 primary payload octets)`.
Only the key varies: the seed for a canonical derived CUID, the temporary fixed
key for an auxiliary one.

*Alternative considered:* the library's current temporary path, which keys with
the primary payload and passes a fixed application UUID as the message, on the
argument that the machine ID inside the primary is what needs protecting.

*Rejected because* the argument does not hold. With a public key, HMAC is a
keyed hash; its preimage resistance protects the message whichever way round the
operands go. An attacker who can enumerate candidate machine IDs can test them
against either construction at the same cost. What the swap does buy is a second
code path, a second thing to get wrong, and — as actually happened — a derived
auxiliary CUID that reads bit 117 out of the fixed constant and so is never
marked auxiliary.

### D4 — Fixed-width binary input structure for the auxiliary serial

S1's 256-bit table is kept, with two field boundaries repaired so the widths sum
to 256 (`0:15` Format, `16:143` Machine ID). The serial is the first 8 octets of
the unkeyed SHA-256 of that structure, little-endian.

*Alternative considered:* keeping the library's string-concatenation approach —
BDF string filtered to hexadecimal digits, concatenated with the machine-id
string.

*Rejected because* the filter erases the separators. `0000:65:00.0` and
`0000:65:0:00.0` reduce to the same input, and the CPU variant's `"socket:"`
prefix degenerates to the constant `cce`. A fixed-width binary structure cannot
fail that way, and it is what S1 describes; only its arithmetic was wrong.

*Alternative also considered:* keeping the kernel's auxiliary serial (SMBIOS
hash plus domain and routing ID). Rejected because the auxiliary path is
user-mode only and the kernel's copy is being deleted.

### D5 — Vectors are a shared artifact, not a transcription

The vector suite lives in one place and both trees consume it, with a drift
check that fails the build.

*Alternative considered:* writing the same expectations into each producer's own
test suite.

*Rejected because* that is what exists now and it is how the bug got confirmed
against itself: a reverse-lookup test decoded the component type from the same
wrong bit positions the packer wrote it to, and passed for months. A test that
is derived from the implementation tests nothing.

*Mechanism:* the same shape as the existing SHA-256 drift check in the library's
CI — a script that compares the local copy against the canonical one and fails on
any difference. The canonical copy is a plain text table plus the generating
script, so a reviewer can regenerate it and a spec author can paste it.

### D6 — Platform CUID is the SMBIOS UUID, untouched

*Alternative considered:* the library's current behaviour, which takes the first
8 octets of the system UUID as a 64-bit fingerprint and packs it through the
normal layout with a component type.

*Rejected because* S1 says twice to use the value directly, and because the fold
discards half of an identifier the firmware has already made unique while making
the result depend on which producer folded it. The literal reading does leave the
Platform CUID with no component type and no framing of ours; that is the
specification's intent and it was confirmed rather than assumed.

### D7 — 32 octets for a provisioned seed, and the default is exempt

S1 and S3 both specify a 256-bit shared secret; the library already enforces it;
the kernel accepts anything up to 32. The kernel moves to exact-32.

The canonical fallback seed is 24 octets and stays 24. It is not a provisioned
secret — it is a built-in placeholder whose only job is to make two unprovisioned
layers agree — so the length rule does not apply to it, and lengthening it would
change every unprovisioned derived value for nothing.

*Trade-off accepted:* the two lengths look inconsistent side by side. The
alternative is either changing values or weakening the rule that catches a
corrupt key file.

### D8 — S4 is retired, not restored

S4's entire body is "Duplicated from S1" plus a stray character, and it is the
page S1's reply links to as the authority for the bit tables. S3 also cites a
section of it that no longer exists.

*Decision:* point S3 at S1 and retire S4. *Alternative:* restore S4 by copying
S1's tables into it — rejected, because a duplicated normative table is exactly
how S1 and S3 got out of step on the device-type enumeration. One page owns the
tables.

## Risks / Trade-offs

- **Renumbering the library's device-type enumeration is a published-API break,
  and every library-emitted GPU CUID changes.** → The current values are wrong in
  a way that misnames the hardware: a GPU ships as type `3`, which a conforming
  reader decodes as a NIC. There is no version of this that does not change those
  values. Land it before the library has consumers that have recorded CUIDs,
  and pair it with the enumeration-typed interface so a raw integer cannot be
  passed after the change.

- **The two constants are being fixed without the answer that was asked for.** →
  Both are fixed at the values already shipping in both layers, so the decision
  is a no-op against the current state and can be replaced later by editing one
  constant in each producer plus the vectors. The vectors make the replacement
  mechanical rather than archaeological. If the requested values ever arrive and
  differ, the cost is one re-derivation, which is the same cost as any re-key.

- **The auxiliary construction is specified in more detail than S1 states.** →
  Unavoidable: S1's version cannot be implemented as written, because its ranges
  do not sum. The repair is minimal and the two changed boundaries are called out
  explicitly so a spec author can apply exactly them.

- **The vector suite pins placeholder constants into a normative artifact.** →
  The derived vectors are split deliberately: D-1 uses the canonical seed and
  will change if that constant ever does; D-2 uses `00..1f`, which is not a
  placeholder and pins the fold, the framing and the operand order independently
  of any constant that might move.

- **A drift check across two repositories is a coupling that can rot.** → It is
  the same mechanism already in use for the SHA-256 sources in the library's CI,
  which has held. The failure mode of not having it is the one this change exists
  to fix.

### D10 — The PCIe Device Serial Number is little-endian, unswapped

Config space is defined little-endian and the DSN capability's first serial
dword is the low half, so the eight octets read at `dsn_cap_offset + 4` are a
64-bit little-endian value and payload bits 0:63 carry it verbatim. The kernel
already did this, via `pci_get_dsn()`. The library byte-swapped.

*Alternative considered:* keep the swap in the library and change the kernel to
match, on the argument that the library's values are already in the field.

*Rejected because* the swapped value has no defensible reading — it is neither
the number the capability holds nor the number any other tool prints for the
same card — and the kernel's orientation is the one `lspci -vv`, `pci_get_dsn()`
and the PCIe base specification all agree on. Reversal cost: every
DSN-sourced library CUID changes once. Nothing that was correct changes, because
nothing that was swapped was correct.

*Detection cost of not deciding:* zero. Both layers produced well-formed UUIDv8
values from a real serial. Only a side-by-side comparison of the two layers on
one card shows it, which is the comparison that has been run once, by hand, in
the format's whole life.

### D11 — A NIC's permanent MAC address needs an orientation, not permission

A NIC with no driver serial, no DSN and no vendor-specific capability falls back
to its permanent MAC address, octet 0 at payload bits 0:7.

S1 already permits this in two places — component type 3 in the primary table
("If no Device Serial Number is defined, then the MAC address value can be used
alternatively for that field") and the general PCIe section. So the decision is
not whether the source is allowed. It is that neither place says which end of
the address goes at bit 0, and an unstated orientation is exactly how the Device
Serial Number came to be byte-swapped in one layer and not the other (D10).

*Alternative considered:* drop the fallback and let such a NIC take an auxiliary
CUID.

*Rejected because* a burned-in MAC is a genuine per-device serial and an
auxiliary CUID is explicitly not one — it is unstable across an OS reinstall and
not unique across nodes. Trading a real identity for a synthetic one to keep the
source list short is the wrong direction. What the short source list did cost is
that the orientation was unstated, and an unstated orientation is precisely how
D10 happened.

### D12 — The driver stage belongs to the bus, not to amdgpu

The staged lookup's first stage is attempted for every component with a PCI
routing ID — GPU, NIC and NPU alike — rather than only for the GPU, whose driver
is the only one publishing the attributes today.

*Alternative considered:* leave the driver stage on the GPU path, where the only
producing driver is, and generalise when a second one appears.

*Rejected because* the generalisation is a base-class method rather than a
feature, and the failure mode of deferring it is silent: the day an NPU driver
publishes a CUID, the library keeps computing its own and the two disagree with
nothing to say so. That is the exact failure the staged lookup exists to
prevent, so it should not be re-introduced for two of the three component types.
A component with no routing ID falls through at the first check, so the CPU and
the platform pay nothing for it.

### D13 — The serial-only Platform branch carries no vendor

The else-branch of the Platform CUID — SHA-256 of the SMBIOS system serial, when
there is no system UUID — packs UnitID, Revision, Device and Vendor as zero, as
the specification says. The library was packing an SMBIOS-derived vendor ID into
bits 96:111.

*Rejected because* a platform is not a PCI function and has no vendor ID; the
value being packed was a property of whoever assembled the machine, so two
producers reading the same firmware could reasonably disagree on it, and the
field is not what identifies the platform in any case. The serial digest is.

## Migration Plan

1. **Library, format-affecting, one commit** — the device-type renumbering, the
   component-type high-bit packing, the framing's last octet, the temporary-key
   operand order and key, and the Platform passthrough. These must land together:
   changing the packing without the framing collides an NPU with a Platform, and
   the vectors will catch that but only if all four move at once.
2. **Vectors into both trees**, with the drift check, before anything else can
   regress.
3. **Kernel, one line** — the seed length check.
4. **Library, kernel consumption** — the staged lookup. This is the durable fix;
   until it lands the two layers can still disagree in the field, they just now
   have a test that says they should not.
5. **Specification edits** — S1's four corrections, S3's enumeration, S4's
   retirement. Independent of the code and can proceed in parallel.

Rollback: steps 2, 3 and 4 are independently revertible. Step 1 is not, in the
sense that reverting it restores identifiers that name the wrong device class.

## Open Questions

- Where the shared vector artifact physically lives. Either tree can host it with
  the other checking out a copy; the drift mechanism is the same either way, and
  the choice does not change any specification or task.
- Whether the CPU Format's auxiliary structure should carry the APIC or package
  identifier in the currently-reserved bits `220:255`. Not needed for the
  components in scope, and additive within a reserved field, so it can be
  answered later without invalidating anything.
