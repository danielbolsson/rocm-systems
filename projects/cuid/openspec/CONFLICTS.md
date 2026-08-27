# Conflict register

Every place the published specification contradicts itself, contradicts a
standard it cites, or leaves a value an implementer must invent.

`specs/cuid/` records each one where it occurs, marked
`Recorded contradiction` / `Recorded defect` / `Recorded gap`. Every one is
resolved in `changes/amend-published-cuid-spec/`. `openspec/check_conflicts.py`
verifies that mapping and fails if a conflict loses its resolution.

Source: "Persistent platform component identification for SW tools", version 84.

| ID | Conflict | Recorded in | Resolution |
|---|---|---|---|
| C1 | A firmware-supplied UUID is "used directly" as the PrimaryID, but every CUID must be a UUIDv8. An SMBIOS system UUID is version 1, 3 or 4 — never 8. | `primary-identifier` | Two constructions, distinguished by the version nibble: **constructed** (payload, always v8, decodable) and **adopted** (firmware UUID verbatim, opaque). Consumers must not reject on the nibble. |
| C2 | The HMAC message is "the primary ID 122bit wide value". 122 bits is not a whole number of octets, so this does not name a byte string. | `derived-identifier` | The message is the sixteen packed octets, LSB first, padding bits present and zero. |
| C3 | The auxiliary serial must be "64bit wide" and the input structure is 256 bits; the reduction is never stated. | `auxiliary-fallback` | First 8 octets of unkeyed SHA-256 over the structure, little-endian. |
| C4 | The auxiliary input is described twice and incompatibly — an unordered field list with an `amd.com` namespace, and a fixed-width 256-bit structure. | `auxiliary-fallback` | The structure is the sole input. No namespace, no field list. |
| C5 | A DerivedID must be "constant for the lifetime of the lesser-privileged SW context", and is also a function of a salt that may change. | `identifier-model` | Constant **while the salt is unchanged**; re-keying is an administrative invalidation, and the association is recorded. |
| C6 | Scalability requires no collision "anywhere"; the fallback section says auxiliary inputs are "not guaranteed to be unique across cluster nodes". | `identifier-model` | The guarantee is scoped to canonical CUIDs. Auxiliary values are marked by bit 117 so a consumer can tell before relying on one. |
| C7 | The collision bound of 1/(2^(122-1)) assumes 122 varying bits; the derived payload carries at most 109. | `derived-identifier` | Bound restated over the 109 hash bits. No value changes. |
| C8 | The auxiliary Component Type field admits only GPU, NIC and NPU, while the primary layout defines sixteen types. | `auxiliary-fallback` | Full on-wire enumeration; the fallback is not a property of component type. |
| C9 | The CPU auxiliary table renames bits `200:215` from VendorID to FamilyID and splits Family/Model, disagreeing with both the PCIe structure and the primary layout. | `auxiliary-fallback` | One structure, one meaning per field. Device holds Family+Model combined, Vendor holds the vendor. |
| C10 | Bit 117 is UnitID part 2 in the primary table and the Auxiliary Value Identifier in the derived table. | `primary-identifier`, `derived-identifier` | UnitID narrows to `112:116`, 13 bits total. Bit 117 is the auxiliary marker in both layouts. |
| C11 | The derived hash slot is 45 bits wide, labelled `hash[64:109]` (46), inside prose saying 110. | `derived-identifier` | `hash[64:108]`, 45 bits, 109 hash bits total. The slot width was already right. |
| C12 | Auxiliary CUIDs are specified as UUIDv5 with an `amd.com` namespace, using HMAC-SHA-256 in place of SHA-1 — which is not a conforming UUIDv5. | `auxiliary-fallback` | Withdrawn. Uniform UUIDv8 distinguished solely by bit 117. |
| C13 | The auxiliary input structure gives Format 17 bits and Machine ID 127; neither is a whole number of octets and the Machine ID cannot hold `/etc/machine-id`. | `auxiliary-fallback` | Format `0:15`, Machine ID `16:143`. |
| C14 | The fallback prose calls the Linux Machine ID a "32bit MachineID". `/etc/machine-id` is 128 bits. | `auxiliary-fallback` | Corrected to 128 bits. |
| C15 | The CPU auxiliary structure retains a PCIe Routing ID field for a component with no Bus/Device/Function. | `auxiliary-fallback` | Zero. |
| C16 | A constant seed key is permitted for auxiliary derivation but never given. | `auxiliary-fallback` | `AMD-CUID-TEMP-KEY-v1`, 20 ASCII octets, unpadded, key not message. |
| C17 | No canonical fallback seed is named, so an unprovisioned machine's derived CUID is undefined. | — | `AMD-CUID-DEFAULT-SEED-v1`, 24 ASCII octets, unpadded. |
| C18 | The PCIe Device Serial Number's byte order is never stated. | — | Configuration-space order, little-endian, unswapped, from `dsn_cap_offset + 4`. |
| C19 | The NIC MAC fallback is permitted but its orientation is never stated. | — | Octet 0 at payload bits 0:7; an all-zero address is absent. |

## Which of these had already shipped as a defect

C10, C12, C13 and C18 each produced a wrong value in shipped code. C1 produced
three: a Platform reported as an NPU, a firmware identity reported as
synthesised, and two platforms differing only in version bits deriving the same
secondary CUID.

The rest were caught by writing the specification down and adding up the field
widths, which is the argument for having done it.
