## MODIFIED Requirements

### Requirement: Primary payload layout for PCIe devices

The PrimaryID payload for a PCIe device SHALL be 122 bits, laid out as:

| Bit | Name | Comments |
|---|---|---|
| 0:63 | Device / Platform / Component / CPU serial number | As published |
| 64:71 | UnitID (part 1) | Low bits |
| 72:79 | RevisionID | |
| 80:95 | DeviceID | |
| 96:111 | VendorID | |
| **112:116** | **UnitID (part 2)** | **High bits — five, not six** |
| **117** | **Auxiliary Value Identifier** | **Named in this layout too, not only in the derived one** |
| 118:121 | Component Type | |

UnitID SHALL therefore be **13 bits** in total, split across bits `64:71` and
`112:116`.

Bit 117 SHALL be the Auxiliary Value Identifier in the Primary layout and in the
Derived layout alike, and SHALL NOT be part of UnitID.

*The published table gives UnitID part 2 six bits, `112:117`, while the Derived
table gives bit 117 to the Auxiliary Value Identifier. Both cannot hold. UnitID
yields the bit, for two reasons: the Auxiliary Value Identifier has no other
home, whereas UnitID merely becomes 13 bits instead of 14; and a producer that
kept the bit for UnitID would set the auxiliary marker on any device whose
UnitID happened to exceed 4095, silently mislabelling a canonical identifier as
synthesised.*

#### Scenario: UnitID is 13 bits

- **WHEN** a UnitID of `0x1FFF` is packed
- **THEN** it round-trips exactly
- **AND** bit 117 is unaffected

#### Scenario: A large UnitID does not set the auxiliary marker

- **WHEN** a component with a UnitID above 4095 is packed
- **THEN** bit 117 remains clear

#### Scenario: A serial shorter than 64 bits

- **WHEN** the architectural serial is narrower than 64 bits
- **THEN** it is MSB zero-extended into bits 0:63

#### Scenario: A serial longer than 64 bits

- **WHEN** the architectural serial is wider than 64 bits
- **THEN** it is truncated

#### Scenario: A component-provided UUID pre-empts the layout

- **WHEN** an ACPI device object or an SMBIOS entry already provides a UUID
  specific to the individual component
- **THEN** that UUID value is used as the PrimaryID directly

### Requirement: A component-provided UUID is adopted, not constructed

Where an ACPI device object or an SMBIOS entry already provides a UUID specific
to the individual component, that UUID SHALL be the PrimaryID, used verbatim.

Such an identifier is **adopted**. It SHALL retain the version and variant bits
its source wrote, which in practice are version 1, 3 or 4 and never 8. Its
payload fields — Component Type, Vendor, Device, Revision, UnitID and the
Auxiliary Value Identifier — SHALL NOT be decoded from it, because it has none:
the bits in those positions are whatever firmware wrote.

Where an adopted identifier is derived from, the HMAC message SHALL be the
sixteen octets of the UUID itself. A producer SHALL NOT de-frame it as though it
were a 122-bit payload first.

*The published text says "used directly" and separately requires every CUID to
be a UUIDv8. Both cannot hold, and "used directly" is the one worth keeping: the
firmware value is already unique and reframing it would make the Platform CUID
depend on the producer's transform rather than on the platform.*

*This is a narrower carve-out than it looks, and deliberately narrower than the
UUIDv5 auxiliary path this change withdraws. That path had the same 122-bit
payload structure and merely tagged it with a different version — two type tags
for one construction, which bought nothing. An adopted identifier has no CUID
payload at all. The version nibble is therefore doing real work here: it tells a
consumer whether there are fields to read.*

*Measured consequence of not stating this. De-framing real SMBIOS UUIDs as CUID
payloads yielded a Component Type of `0xB` (Reserved), `0x4` (NPU) and `0xF`
(Other) for three platforms, and set the Auxiliary Value Identifier on one of
the three — a genuine firmware identity reported as synthesised. De-framing also
does not round-trip: it drops six of the UUID's bits, so two platforms whose
system UUIDs differ only in their version and variant bits de-frame to the same
payload and derive the same secondary CUID. That is a collision manufactured by
the producer out of an identifier the firmware had already made unique.*

#### Scenario: The firmware UUID is preserved exactly

- **WHEN** SMBIOS reports a system UUID
- **THEN** the Platform CUID is those sixteen octets, including the firmware's
  own version and variant bits

#### Scenario: No field is decoded from an adopted identifier

- **WHEN** a consumer holds a Platform CUID sourced from firmware
- **THEN** it reports the Component Type and the auxiliary marker as not
  applicable rather than decoding them

#### Scenario: Derivation hashes the whole UUID

- **WHEN** a secondary CUID is derived from an adopted primary
- **THEN** the HMAC message is the sixteen octets of the UUID
- **AND** two platforms differing only in version or variant bits derive
  different secondary CUIDs
