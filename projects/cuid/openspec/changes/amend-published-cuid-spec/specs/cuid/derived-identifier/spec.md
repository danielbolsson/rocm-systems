## MODIFIED Requirements

### Requirement: Derived payload layout

The 122 bits of the DerivedID value SHALL be laid out as:

| Bit | Name | Comments |
|---|---|---|
| 0:63 | `hash[0:63]` | LSB of the digest |
| 64:71 | Reserved | Must be 0 |
| **72:116** | **`hash[64:108]`** | **45 bits, matching the width of the slot** |
| 117 | Auxiliary Value Identifier | Copied from the primary |
| 118:121 | Reserved2 | Must be 0 |

The DerivedID SHALL therefore carry **109** hash bits, not 110.

*The published table labels a 45-bit slot `bits [64:109]`, which is 46 bits, and
the prose around it says 110. The slot width is the value that was implemented
and the one that leaves room for bit 117, so the label and the prose are what
move. No conforming value changes.*

#### Scenario: The derived slot is 45 bits

- **WHEN** a derived payload is compared against its digest
- **THEN** payload octet 14 is digest octet 13 masked to its low five bits
- **AND** the sixth bit of that octet is the Auxiliary Value Identifier

#### Scenario: The reserved fields are zero

- **WHEN** a DerivedID payload is decoded
- **THEN** bits 64:71 and 118:121 are zero

#### Scenario: Collision resistance

*Carried from the baseline with the figure corrected: the baseline recorded the
published 1/(2^(122-1)), and "The collision bound follows the varying bits" in
this same change is what restates it over the 109 hash bits the table above
carries. No conforming value changes.*

- **WHEN** the chance of an accidental collision is computed
- **THEN** the bound is 1/(2^(109-1)), taken over the 109 hash bits, not
  1/(2^(122-1))

### Requirement: Derivation by HMAC-SHA-256

Unchanged in operand order and algorithm. The key SHALL additionally be
specified as follows.

A provisioned shared salt SHALL be **exactly 32 octets**. A producer SHALL
reject any other length and SHALL continue to use the previously effective key
rather than adopting a truncated or padded one.

Until an administrator provisions a secret, a producer SHALL key the derivation
with the **canonical fallback seed**, which is exactly the 24-octet ASCII string
`AMD-CUID-DEFAULT-SEED-v1`, with no terminating NUL and no padding to any other
length.

The fallback seed is public. A derived CUID produced under it is stable and
reproducible but SHALL NOT be treated as fleet-unique, and SHALL be documented
as a placeholder.

*HMAC-SHA-256 pads a short key to its own 64-octet block internally, so padding
to 32 first is not an HMAC operation but a different key. The value has to be
stated because two independent implementations otherwise each invent one, and an
unprovisioned machine then reports two different derived CUIDs for the same
component depending on which layer is asked.*

#### Scenario: The operands

- **WHEN** a DerivedID is generated
- **THEN** the PrimaryID payload is the message and the shared salt is the key

#### Scenario: The salt is 256 bits

- **WHEN** the shared secret is established
- **THEN** it is 256 bits

#### Scenario: A shared salt gives a shared answer

- **WHEN** two nodes carrying the same salt derive from the same PrimaryID
- **THEN** they produce the same DerivedID

#### Scenario: An unprovisioned kernel and library agree

- **WHEN** both are asked for the derived CUID of the same component on a
  machine with no provisioned seed
- **THEN** both key with the same 24 octets and emit the same value

#### Scenario: A wrong-sized salt is refused

- **WHEN** a 16-octet salt is provisioned
- **THEN** the producer reports an error and the derived CUID is unchanged

### Requirement: The HMAC message is sixteen octets

The message SHALL be the **sixteen octets** into which the 122-bit primary
payload is packed, LSB first, with payload bits 122:127 present and zero.

*HMAC-SHA-256 consumes octets. The published text gives the message as "the
primary ID 122bit wide value", which is not a whole number of octets and so does
not name a byte string. A producer that fed 122 bits some other way — MSB-first,
or right-aligned in the last octet — would emit values sharing no bits with a
conforming one, and nothing in either value would show which was which.*

#### Scenario: The message is the packed payload

- **WHEN** a derived CUID is computed
- **THEN** the HMAC message is the same sixteen octets the primary CUID is
  framed from

#### Scenario: The padding bits are part of the message

- **WHEN** the message is inspected
- **THEN** its final six bits are the payload's zero padding

### Requirement: The collision bound follows the varying bits

The collision bound for a DerivedID SHALL be stated over the **109 hash bits**
it carries, not over 122.

*The published figure of 1/(2^(122-1)) assumes the DerivedID is "built from 122
custom bits of the hash value". Thirteen of those 122 are reserved or the
auxiliary flag and are fixed across every value, so they contribute nothing to
collision resistance. The correct bound is over 109 bits.*

*This changes no value and no code. It is corrected because a bound overstated
by thirteen bits is the kind of thing that gets quoted into a security review,
and 2^109 is ample for the stated purpose without needing to be overstated.*

#### Scenario: The bound is over what varies

- **WHEN** the collision bound is computed
- **THEN** it is taken over the 109 hash bits the derived payload carries
