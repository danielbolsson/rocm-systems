## Context

`openspec/specs/cuid/` records the published specification at version 84. This
change is the delta between that and what two independent implementations had to
agree on in order to emit the same value for the same card.

The implementations already exist and already agree:

- the kernel driver, `drivers/gpu/drm/amd/amdgpu/amdgpu_cuid.c`;
- the userspace library, `projects/cuid` in rocm-systems;
- and `amd-smi`, which consumes the library.

All three are verified against a shared conformance-vector artifact, and against
two Radeon Pro W6800s. So the question this change answers is not "what should
the format be" — that is settled and running — but "what does the page have to
say so that the next implementation reaches the same answer without three
defects on the way".

## Goals / Non-Goals

**Goals:**

- Remove every contradiction the page has with itself, so that a conforming
  implementation is possible at all.
- State every value an implementer is currently forced to invent.
- Leave a page that agrees with the code, so the two stop drifting.

**Non-Goals:**

- Changing any emitted value. Every edit here describes what both
  implementations already do; the one exception is called out below.
- Redesigning the format. The two-level model, the UUIDv8 framing, the HMAC
  derivation and the component-type enumeration are all kept exactly as
  published.
- The virtualization and out-of-band sections. They are internally consistent
  and nothing implemented so far contradicts them.

## Decisions

### E1 — UnitID yields bit 117, not the Auxiliary Value Identifier

The Primary table and the Derived table both claim bit 117. One has to give.

*Alternative considered:* keep `112:117` for UnitID and move the Auxiliary Value
Identifier into the Reserved field at `118:121` of the derived layout.

*Rejected because* `118:121` is Component Type in the Primary layout, so the
auxiliary marker would occupy a different bit in the two layouts and could not
be carried across the derivation by a single mask — which is how every
implementation carries it. It would also mean a derived value could not be
identified as auxiliary without reference to its primary.

The cost of the chosen direction is that UnitID becomes 13 bits rather than 14,
capping a partition index at 8191. No AMD component subdivides anywhere near
that far.

There is also a positive argument. A producer that kept the bit for UnitID sets
the auxiliary marker on any device whose UnitID exceeds 4095 — silently
relabelling a canonical identifier as synthesised. The failure is invisible in
the value.

### E2 — The slot width wins over the label and the prose

The derived hash slot is described three ways: 45 bits by position, 46 by label,
110 by prose.

The position is authoritative because it is the only one of the three that is
consistent with the rest of the table — 64 + 45 + 1 + 4 + 8 = 122 exactly. The
other two readings each overflow the payload once bit 117 is accounted for.

*Reversal cost:* none. Both implementations already carry 109 bits.

### E3 — Uniform UUIDv8, and the auxiliary path loses its namespace

*Alternative considered:* keep auxiliary CUIDs as UUIDv5 as published, and widen
the derived slot to 46 bits since bit 117 would no longer be needed.

*Rejected because* it is an ABI break with nothing on the other side of it. A
v5-typed value carrying an HMAC-SHA-256 payload is not a conforming v5 UUID, so
a validating parser may reject it and some libraries re-canonicalise on it. It
would split every consumer's parser in two, and it would make the UUID version
of a device depend on the privilege of the caller and the host it happens to be
enumerated on — which is a property of the environment, not of the device.

Both packages were acceptable to the page's author. This one is what the kernel
and S3 already implement.

*Consequence:* the `amd.com` namespace string and the namespace form cease to
exist as questions rather than being answered, which is why the proposal lists
them as voided rather than changed.

### E4 — State the constants, even though they are placeholders

Two answers were requested and did not arrive: the canonical seed's exact bytes
and the temporary key's. Both are placeholders in both layers today, and both
are byte-identical across the two layers today.

The decision is to write down what is already shipping rather than wait. Writing
it down changes no emitted value; not writing it down means the third
implementation invents a third value, and an unprovisioned machine then reports
different derived CUIDs depending on which layer is asked. If the constants are
later changed deliberately, that is a versioned decision with a migration — much
better than an accidental divergence nobody can see.

### E5 — Orientation is part of a field definition

The page gives the Device Serial Number's capability ID and width but not its
byte order, and gives the NIC MAC fallback but not its orientation. Both gaps
produced divergence.

The general principle this change adopts: **a field definition that does not fix
the byte order is not a field definition.** Every multi-octet field in the
amended text says which end is which.

## Risks / Trade-offs

- **Narrowing UnitID is technically breaking.** → Only for a producer that
  packed a UnitID above 4095, which none does. And the alternative — leaving the
  contradiction — means no producer can be conforming at all.

- **Publishing placeholder constants makes them harder to change later.** →
  Mitigated by stating explicitly that the fallback seed is public and a
  placeholder, and by the conformance vectors being split so that one of the two
  derived vectors uses a reproducible test key rather than the placeholder. If
  the constant moves, exactly one vector moves with it.

- **The page and the code can drift again.** → The conformance vectors are the
  mitigation, and they are the reason task 4.1 exists. A vector in the page is
  checkable; a table in the page is not.

- **Retiring S4 loses history.** → S4 is empty. What it cost was being cited by
  S1's own reply as the authority for the bit tables, which is how the two got
  out of step.

## Open Questions

- Whether the specification should describe a hypervisor opt-out at all. amdgpu
  now implements one as a module parameter, on the reasoning that a GIM-level
  disable leaves bare metal and non-GIM hypervisors with no lever. Whether that
  belongs in the page, or is properly an implementation matter, is not settled.
- Whether the CPU auxiliary structure should carry the APIC or package
  identifier in the reserved bits `220:255`. Additive within a reserved field,
  so it can be answered later without invalidating anything.
- Where the shared conformance-vector artifact should physically live so that
  the page, the kernel tree and the library tree all reference one copy.
