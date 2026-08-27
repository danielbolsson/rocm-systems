## Context

See `proposal.md` — Why. The design-relevant constraints:

- **Two implementations already exist and must agree.** The kernel driver and
  the ROCm CUID library (`projects/cuid`) both produce CUIDs today. They have
  diverged three times — mirrored UUIDv8 octet order, an off-by-one component
  type enumeration, and independently dropping payload bits 120:121 — and each
  divergence was found by hand rather than by a test. Cross-layer verification
  on two W6800s now shows byte-identical primary and secondary values. Keeping
  that true is a design constraint, not a follow-up.
- **The specification is under active revision and is internally
  inconsistent.** S1 v84's primary table gives `112:117` to UnitID part 2 while
  its derived table claims 117 as the Auxiliary Value Identifier; the derived
  table allocates 45 bit positions for 46 hash bits while the prose says 110.
  These cannot all stand. The kernel resolves them one specific way (below) and
  the resolution has to be written down here, because the published text does
  not yet say it.
- **The auxiliary/fallback construction is not implementable as published** and,
  as of 2026-08-20, is settled as user-mode-only work. The in-kernel fallback
  path is therefore removed rather than corrected.
- **Sysfs is a stable ABI.** Once posted, the attribute names, permissions,
  encodings and the identifier format itself are frozen. Anything uncertain must
  be left out rather than shipped provisionally.
- **The code is MIT-licensed** and lives under `drivers/gpu/drm/amd/amdgpu/`,
  but the derivation needs `hmac_sha256_usingrawkey()`, which is
  `EXPORT_SYMBOL_GPL`.

## Goals / Non-Goals

**Goals:**

- One implementation of the packing, framing and derivation, with no
  driver-specific types in it, so AINIC and NPU can adopt it unchanged later.
- A resolution of every specification ambiguity that affects an emitted byte,
  recorded with its reasoning, so the resolution survives the next spec revision
  rather than being re-litigated.
- No failure path on the read side: identifiers computed once at bind and served
  from cache.
- A privilege split that is enforced per-read, not only by file mode.

**Non-Goals:**

- Seed persistence, node-wide seed scope, IOCTL access, per-partition UnitID,
  and the auxiliary construction — all deferred, see `proposal.md`.
- A standalone `amd_cuid.ko`. The core is written to be extractable, but
  extraction is not attempted here.
- Changing the userspace library. It is the other half of the ABI, and the
  shared test vectors are the contract; the library work is tracked separately.

## Decisions

### D1 — The core is driver-independent, called through an input descriptor

The caller fills a `struct cuid_ident` (PCI device, serial, vendor/device/
revision IDs, UnitID, component type) and embeds a `struct cuid` in its own
device structure; the core owns the `device_attribute`s inside that struct and
recovers it from `container_of()` in the show/store handlers.

*Why:* the CUID specification covers platforms, CPUs, GPUs, NICs, NPUs and
storage. Three AMD drivers are expected to publish CUIDs. Passing an
`amdgpu_device *` would have to be undone before the second caller. The
descriptor carries no driver types, so extraction into a shared module is later
a file move rather than a rewrite.

*Alternative rejected:* ops-vtable callbacks so the core pulls identity from the
driver on demand. More indirection for no benefit — the identity is fully known
at bind time and never changes.

### D2 — Bit 117 is permanently the Auxiliary Value Identifier; UnitID part 2 is `112:116`

S1's two tables disagree. The kernel packs `112:116` for UnitID and reserves 117
for the auxiliary marker in **both** the primary and the derived layout, and the
derived hash slice is correspondingly `hash[64:108]` — 45 bits, not 46.

*Why:* the alternative — retiring bit 117 and returning it to UnitID, with the
auxiliary CUID distinguished by a UUIDv5 version nibble instead — was proposed
and is acceptable to the specification's author, but:

1. **A v5-typed CUID is not a valid v5 UUID.** RFC 9562 defines version 5 as
   SHA-1 over `namespace ‖ name`; the proposal is HMAC-SHA-256. A validating
   parser is entitled to reject it, and some UUID libraries re-canonicalise on
   unknown-version input. A v8 value makes no claim a checker can falsify —
   which is what v8 is for, and what S1's own Compatibility requirement already
   demands.
2. **Mixed versions split every consumer.** amd-smi, rocprofiler-sdk, the
   debugger and this interface would each need a branch on the version nibble
   before parsing the payload. One bit inside a single fixed layout needs a mask,
   not a branch.
3. **The same device would present different versions by privilege and by
   host** — v8 where the kernel has a serial, v5 where it does not. A tool
   comparing identifiers across nodes would see a type change for a property
   that is not about the device at all.
4. **It costs nothing and the alternative is an ABI break.** The kernel and the
   library already implement this, and S3 already documents bit 117. The
   alternative invalidates every derived CUID emitted so far.

*Consequence to carry back to the specification:* S1's primary table must be
corrected to `112:116`, its derived slice renumbered to `hash[64:108]`, and its
"110 bit" prose dropped to 109. The off-by-one is real in either branch — this
choice only fixes its direction.

### D3 — UUIDv8 framing inserts six bits rather than overwriting

The payload is emitted in order from octet 0, with the version nibble and the
two variant bits shifted in, displacing everything after them. Payload bits
122:127 fall off the end and are always zero, so nothing is lost and the
transform is exactly invertible.

*Why this orientation:* S1's representation table pins it. The version at bits
48:51 and the variant at 64:65 are the RFC 9562 positions only under MSB-first
numbering over the rendered octets, and that same table then places "ID value
part 1, LSB of ID Value" at bits 0:47 — the front of the string. So the least
significant payload octet leads. The kernel originally scattered payload bit 0
to the last octet, producing a mirror image of the library's value for the same
input; the kernel was the odd one out and was corrected.

*Alternative rejected:* overwriting the six bits with the framing, which is what
a naive `uuid` helper does. It would silently discard two component-type bits
and make the value non-invertible.

### D4 — Remove the in-kernel auxiliary/fallback path entirely

`cuid_aux_serial()`, the DMI/SMBIOS lookup it performs, the `aux` argument
threaded through the packing routine and the `cuid_temporary` attribute all go.

*Why:* the auxiliary CUID is user-mode-only by decision. A kernel-side temporary
CUID would disagree with the library's *by construction*, because the library's
algorithm is defined over `/etc/machine-id`, which a kernel driver cannot read.
Shipping an attribute whose value no other layer can reproduce, and then
deprecating it, is worse than never shipping it. Removing it also deletes the
kernel's only dependency on DMI.

*Alternative rejected:* keeping `cuid_temporary` and defining an in-kernel
equivalent of the machine ID. That was the objection; it dissolves by deleting
the path, not by finding a substitute.

### D5 — Compute at bind, cache, serve from cache

Primary and derived values are computed in the init path and stored. The show
handlers only format. The derived value is recomputed synchronously inside the
seed write.

*Why:* it puts the only fallible work (attribute group creation) in the probe
path, where failure can be reported, and leaves the read path infallible. It
also removes any question of doing crypto under a sysfs read with unknown
context.

*Enabled by:* the synchronous library HMAC (`crypto/sha2.h`,
`hmac_sha256_usingrawkey()`) rather than the async crypto API — no allocation,
no request objects, no error return.

### D6 — Mutex around the seed, and only the seed

`seed_lock` guards the seed bytes, its length, and the cached derived UUID.
Primary is immutable after init and needs no lock. The secondary show handler
takes the lock, copies 16 bytes out, drops it, and formats outside.

*Why:* the critical section stays a `memcpy`. A reader concurrent with a re-key
sees the old or the new value, never a torn one, which is what the spec
requires.

### D7 — Capability checked on every read, not just the file mode

`cuid_primary` and `cuid_seed` are `0400`/`0600` *and* call `capable(CAP_SYS_ADMIN)`
in their handlers.

*Why:* file mode is checked at open. A descriptor can be opened under one set of
credentials and read under another, and a container or a relaxed sysfs mount can
make the path reachable. The serial number is the thing being protected;
belt-and-braces is cheap.

### D8 — The seed round-trips as raw bytes

Read and write use the same encoding: raw bytes, at most 32, no newline, no hex,
no trailing NUL. Read returns whichever seed is in use — provisioned or default.

*Why:* the provisioning flow is a daemon writing a binary secret and a tool
reading back what is in effect. Any text encoding invites a mismatch between the
two directions; the library already handles exactly 32 raw bytes in its key
file. Returning the default rather than an empty buffer means the read always
answers "what key is actually deriving the value I can see".

*Trade-off:* `cat cuid_seed` prints binary. Acceptable for a root-only secret,
and documented.

### D9 — The association record is a `dev_info()` log line

S1 requires system software to track the derived↔primary association "with a
timestamp in a log". The driver emits one `dev_info()` naming both values at
bind and again on every re-key.

*Why:* it satisfies the requirement literally, at the point where the
information exists, with a timestamp supplied by the kernel. The specification's
separate suggestion of "a consistent API" is a recommendation, and the atomicity
concern raised against it belongs to the provisioning daemon's design rather
than the driver's.

## Risks / Trade-offs

- **The specification may be revised against these resolutions (D2).** →
  Every resolution is written back to the specification owners as a concrete
  edit with replacement text, not as a question. The direction chosen is also
  the reversible one: it changes no already-emitted value, whereas the
  alternative invalidates all of them.
- **The two placeholder constants are not final (T1).** →
  `CUID_DEFAULT_SEED` is byte-identical between kernel and library, so the two
  layers agree today whatever the final value is. It is administrator-overridden
  in any real deployment and documented as a placeholder. Changing it later
  changes only values derived from a seed nobody should be relying on.
- **The layers can silently diverge again.** → The single highest-value
  follow-up is a shared known-answer test: the same vectors bound into the
  kernel-side tooling and the library's suite, so a divergence fails CI. Nothing
  in the current arrangement forces agreement; three defects have already
  escaped because of that.
- **The GPL-only HMAC export against MIT-licensed files.** → Tolerable while the
  object is linked into `amdgpu`. It must be resolved — either a non-GPL export,
  an in-tree implementation, or a licence change — before the core becomes a
  standalone module. Flagged now so it is not discovered at extraction time.
- **The seed is lost on module reload.** → Documented in the ABI file as a
  requirement to re-provision. The alternative, a UEFI variable, cannot ship
  until its GUID, name and attributes are agreed with the other AMD drivers, and
  a world-readable efivar is not an acceptable store for a secret in any case.
- **Cross-instance correlation under virtualisation.** → `cuid_secondary` is
  `0444` in a guest, so two guests on the same physical hardware can determine
  they share a component. The `0400`/`0444` split addresses serial *disclosure*,
  which is a different axis. This is unresolved and is the one open item that
  can still change the ABI; see below.

## Migration Plan

There is nothing deployed to migrate. Against the current out-of-tree branch,
removing `cuid_temporary` is a visible change, but the branch has no consumers —
the library reads none of these files yet.

Deployment is a normal driver update: the attributes appear on bind, the derived
value is immediately readable from the default seed, and a provisioning daemon
writes the real secret when one exists. Rollback is removing the group; nothing
persists across it, since the seed is deliberately not stored.

Sequencing before posting: the userspace library work lands first so there is a
consumer, the shared test vectors land with it, and the hypervisor question
below is answered.

## Open Questions

- **Does a hypervisor/GIM-level disable satisfy PSO, or does `amdgpu` need its
  own opt-out?** S1 comment 454362121 requires that a solution allowing two
  guest instances to fingerprint the same physical hardware "must provide the
  hypervisor the ability to disable the solution", recommended default-off. CUID
  meets that trigger by design. A module parameter is trivial now and an ABI
  change after upstreaming, so this must be answered before posting — it is
  listed here rather than decided because the answer is owned outside this
  change, but it does not block writing the code.
- **Exact bytes of the canonical default seed (T1).** Not blocking: both layers
  ship the same placeholder, and it is overridden in deployment.
- **When does the library start reading these files (T3)?** Determines whether
  the interface has a consumer at the time of posting. Owned by the library
  schedule, not by this change.
