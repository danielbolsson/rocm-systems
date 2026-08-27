## MODIFIED Requirements

### Requirement: Auxiliary CUIDs use UUIDv5

**Withdrawn and replaced.** An Auxiliary CUID SHALL use the same 122-bit payload
layout, the same component type numbering, the same UUIDv8 framing and the same
derivation as any other CUID, and SHALL be distinguished **solely by bit 117**,
the Auxiliary Value Identifier, being set.

A producer SHALL NOT emit an auxiliary CUID with a version nibble other than
`8`, and SHALL NOT use the version nibble to signal that a value is auxiliary.

The `amd.com` namespace string, the namespace form, and the question of HMAC
operand order for the auxiliary path are all **void**: under uniform UUIDv8
there is no namespace.

Three consequences are normative:

- A CUID is never a conforming UUIDv5. RFC 9562 defines version 5 as SHA-1 over
  `namespace ‖ name`; the CUID payload is HMAC-SHA-256 over a defined structure.
  A validating parser is entitled to reject a value that claims version 5, and
  some UUID libraries re-canonicalise on unknown-version input. Version 8 makes
  no claim a checker can falsify, which is what version 8 is for.
- A consumer parses every CUID with one code path and one mask, and does not
  branch on the version nibble before reading the payload.
- The same device presents the same UUID version regardless of the privilege of
  the caller or the host it is enumerated on. Whether a serial was reachable is
  a property of the environment, not of the device, and must not surface as a
  type change.

*The Derived payload table already carries an Auxiliary Value Identifier at bit
117 for exactly this purpose, which makes the UUIDv5 path redundant as well as
non-conforming.*

#### Scenario: A consumer identifies an auxiliary value

- **WHEN** a consumer reads any CUID
- **THEN** it tests payload bit 117
- **AND** the version nibble is `8` in both cases

#### Scenario: The version nibble differs from every other CUID

*Carried from the baseline with its outcome reversed: the baseline asserted the
nibble is `5`, and withdrawing the UUIDv5 path is precisely what this
requirement does. The heading is kept so the loss check can pair the two.*

- **WHEN** an auxiliary CUID is rendered under this amendment
- **THEN** its version nibble is `8`, not `5`, and so does **not** differ from
  every other CUID
- **AND** the value that distinguishes it is payload bit 117, not the nibble

### Requirement: Auxiliary input structure, PCIe device

The input structure SHALL be 256 bits, packed LSB-first into 32 octets:

| Bit | Width | Field |
|---|---|---|
| **0:15** | **16** | **Format** — `1` = PCIe device, `2` = CPU, others reserved |
| **16:143** | **128** | **Machine ID** |
| 144:175 | 32 | PCIe Routing ID |
| 176:183 | 8 | RevisionID |
| 184:199 | 16 | DeviceID |
| 200:215 | 16 | VendorID |
| 216:219 | 4 | Component Type — on-wire numbering |
| 220:255 | 36 | Reserved, SHALL be zero |

The Machine ID SHALL be 128 bits: on Linux, the 32 hexadecimal characters of
`/etc/machine-id` decoded to 16 octets, octet `k` occupying structure bits
`16 + 8k` through `23 + 8k`. Where no machine identity is available the field
SHALL be zero and the producer SHALL treat the resulting identifier as
node-local.

The PCIe Routing ID SHALL be `(segment << 16) | (bus << 8) | (device << 3) |
function`.

Component Type SHALL use the on-wire numbering — the same values as the Primary
payload's Component Type field — rather than the restricted set the published
table lists.

A producer SHALL NOT derive the structure from a formatted string, and SHALL NOT
filter, normalise or otherwise transform the field values before packing them.

*The published ranges total 256, so the error is not arithmetic: Format is
allocated 17 bits and Machine ID 127. A 17-bit field is not a whole number of
octets, and a 127-bit Machine ID cannot hold the 128-bit `/etc/machine-id` that
the same row names. Moving the boundary by one bit fixes both.*

*The prohibition on string-derived input is specific. One producer built its
input by concatenating a BDF string with a machine ID and then removing every
character that was not a hexadecimal digit. That erases the separators, so
`0000:65:00.0` and `0000:65:0:00.0` collapse to the same input, and a CPU seed
of `"socket:" + id` degenerates to the constant `cce` followed by digits.
Fixed-width binary fields have no such failure mode.*

#### Scenario: Both fields are whole octets

- **WHEN** the Format and Machine ID field widths are measured
- **THEN** they are 16 and 128 bits

#### Scenario: Two functions of one device differ

- **WHEN** the auxiliary serial is computed for `0000:65:00.0` and
  `0000:65:00.1` on the same machine
- **THEN** the two Routing ID fields differ, and so do the two serials

#### Scenario: The reserved tail is zero

- **WHEN** the input structure is inspected
- **THEN** bits 220–255 are zero

### Requirement: Auxiliary input structure, CPU

There SHALL be **one** input structure, not two. The CPU case SHALL use the same
field positions and the same per-field meanings as the PCIe case, with Format
`2` and Component Type `1`, and:

- **RevisionID** (`176:183`) SHALL hold the CPUID stepping;
- **DeviceID** (`184:199`) SHALL hold the CPU's Family and Model combined, the
  same way the primary payload defines a CPU's DeviceID;
- **VendorID** (`200:215`) SHALL hold the CPU vendor ID;
- the **PCIe Routing ID field** (`144:175`) SHALL be **zero**.

*A CPU has no Bus/Device/Function of its own. The published CPU table retains
the Routing ID row and says it comes "from PCIe config space", which is not
answerable; zero is the only value two producers can agree on without inventing
one.*

*The published CPU table also renames bits `200:215` from VendorID to FamilyID
and splits Family and Model across two 16-bit fields (C9). That leaves a CPU
auxiliary value carrying no vendor at all, and makes bits `200:215` mean one
thing for a GPU and another for a CPU — the same defect as bit 117 having two
owners, one level down. Collapsing to a single structure with one meaning per
field removes the question, and matches how the primary layout already treats a
CPU.*

#### Scenario: A CPU carries no routing ID

- **WHEN** a CPU's auxiliary input structure is built
- **THEN** bits 144:175 are zero

#### Scenario: A CPU auxiliary value

- **WHEN** a CPU's auxiliary input structure is built
- **THEN** its Format is `2` and its Component type is `1`

### Requirement: Key handling for auxiliary derivation

The constant seed key the published text permits SHALL be exactly the 20-octet
ASCII string `AMD-CUID-TEMP-KEY-v1`, with no terminating NUL and no padding.

The derivation SHALL use the same operand order as every other CUID derivation:
`HMAC-SHA-256(key = the temporary fixed key, message = the 16 auxiliary primary
octets)`. A producer SHALL NOT exchange the operands and SHALL NOT substitute a
fixed constant for the message.

The key is public by construction: an auxiliary CUID is built from
non-privileged information precisely so that unprivileged and out-of-band
consumers can reproduce it, which they cannot do with a secret.

*One producer keyed with the primary payload and passed a fixed application UUID
as the message, on the argument that the message needed protecting. It does not:
with a public key HMAC is a keyed hash, and its preimage resistance protects the
message either way. What the swap cost was a second derivation function and a
real defect — the derivation reads bit 117 out of whatever it is handed as the
primary, so with a constant in that position the derived auxiliary value was
never marked auxiliary.*

#### Scenario: A constant key is permitted

*Carried from the baseline with the key pinned: the permission survives, the
freedom to choose which constant does not.*

- **WHEN** policy allows sharing one derived value across contexts
- **THEN** the constant seed key `AMD-CUID-TEMP-KEY-v1` may be used for the
  auxiliary derivation
- **AND** no other constant may be substituted for it

#### Scenario: The auxiliary marker survives derivation

- **WHEN** a derived CUID is computed from an auxiliary primary
- **THEN** payload bit 117 of the derived value is set

#### Scenario: Independently verifiable

- **WHEN** `openssl dgst -sha256 -mac HMAC -macopt key:AMD-CUID-TEMP-KEY-v1` is
  run over the 16 auxiliary primary octets
- **THEN** it prints the derivation's digest

### Requirement: The reduction from the input structure to the serial

The auxiliary serial SHALL be the **first 8 octets of the unkeyed SHA-256 digest
of the 32-octet input structure, interpreted as a little-endian 64-bit value**,
and SHALL be placed in payload bits 0:63 of an otherwise normal primary payload
with bit 117 set.

*The published text requires a "64bit wide device serial number" and defines a
256-bit input structure, but never states how one becomes the other. Every part
of that reduction is a choice — which hash, truncated from which end, in which
byte order — and a producer that chose differently at any of the three would
emit a value sharing no bits with a conforming one.*

#### Scenario: The serial is a truncated digest

- **WHEN** the auxiliary serial is computed
- **THEN** it is the low 8 octets of SHA-256 over the input structure, read
  little-endian

### Requirement: The input structure is the sole hash input

The 32-octet input structure SHALL be the only input to the auxiliary serial.
There SHALL be no namespace string, and no separate field list.

*The published text describes the input twice: once as an unordered list of
fields combined with an `amd.com` namespace, and once as the fixed-width
structure. The two cannot both be the input. The structure wins because it is
fully specified — every field has a position and a width — whereas the list
specifies neither an order nor an encoding, which is what let one producer
implement it as a concatenated string with the separators stripped.*

*Once auxiliary CUIDs are uniform UUIDv8 the namespace has nothing left to name,
so this removes a question rather than answering one.*

#### Scenario: One input, fully positioned

- **WHEN** two producers build the auxiliary serial for the same device
- **THEN** they hash byte-identical 32-octet structures

### Requirement: Auxiliary Component Type uses the full enumeration

The Component Type field of the input structure SHALL use the same on-wire
numbering as the primary payload, all sixteen values.

*The published table admits only `2` = GPU, `3` = NIC and `4` = NPU, while the
CPU structure uses `1`. A Platform, a Storage device, a Memory module or a
GenPCIe device therefore has no auxiliary encoding at all, which is an arbitrary
restriction: the fallback exists for components whose serial is unreachable, and
that is not a property of the component's type.*

#### Scenario: Every component type can take the fallback

- **WHEN** a Storage device has no reachable serial
- **THEN** its auxiliary input structure carries Component Type `5`
