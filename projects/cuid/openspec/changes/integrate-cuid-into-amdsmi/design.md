## Context

`amd-smi` is the layer an operator touches. Everything below it — the driver's
sysfs attributes, `libamdcuid`'s staged lookup, the conformance vectors — exists
so that one number names one component, and none of it reaches a fleet inventory
unless `amd-smi` reports that number.

The state this change starts from:

- `amdsmi_get_gpu_device_uuid()` is the identity call every tool uses. It builds
  a UUID from `rsmi_dev_unique_id_get()`, the PCI device ID and the KFD partition
  index. It is not a CUID: different inputs, different layout, different framing.
- `amdsmi_get_gpu_device_cuid()` exists, is compiled only when `BUILD_CUID=ON`
  (default `OFF`), returns a single string, and gives the caller no way to learn
  anything about it.
- `libamdcuid` exposes the primary, the derived value, the component type and
  the auxiliary marker through `amdcuid_query_device_property()`, and seed
  provisioning through `amdcuid_set_hash_key()`. Nothing in the library needs to
  change.
- The two projects are separate deliverables with separate release cadences and
  separate CI. They must stay that way.

## Goals / Non-Goals

**Goals:**

- Make the CUID reportable and provisionable through the tool operators already
  run, with enough context attached that a recorded value can be trusted.
- Keep the dependency one-directional, optional, and invisible to a build that
  does not have `libamdcuid`.
- Make the difference between a driver-published identifier and a synthesised
  one impossible to miss, rather than merely discoverable.

**Non-Goals:**

- Merging the projects, vendoring the library, or making `libamdcuid` depend on
  `amd-smi`.
- Changing any value the library or the driver produces. This change reports;
  it does not compute.
- Retiring `amdsmi_get_gpu_device_uuid()`.
- CPU, NIC and platform CUIDs through `amd-smi`. The handle model is per-GPU;
  the node-scoped identities belong to a later change and do not block this one.

## Decisions

### A1 — Supersede `amdsmi_get_gpu_device_uuid()`, do not replace it

The obvious move is to make the existing UUID call return the CUID, so that every
consumer gets the right identifier with no code change. That is the wrong move.

*Rejected because* it silently changes the value of a published ABI. The
consumers — the ROCm runtime's agent enumeration, container device plugins,
Kubernetes device advertisement, anything that has written the value into a
database — would keep reading the same symbol and start getting a different
answer for the same card, with nothing in the type, the length or the format to
show it. A UUID string that changes meaning under a consumer is the single worst
outcome available here, and it is precisely the class of defect the CUID work
exists to eliminate. Doing it in the name of CUID correctness would be
self-refuting.

The two values also do not mean the same thing. The existing UUID is derived from
the SMU unique ID *and the current partition index*, so it changes when a GPU is
repartitioned. A CUID does not: partitioning moves the UnitID field and nothing
else. A consumer that wants "which physical card" and a consumer that wants
"which addressable device" are asking different questions, and both callers
exist.

So: both remain. `amdsmi_get_gpu_device_uuid()` is documented as the legacy
device UUID, the CUID calls are documented as the identifiers to record, and the
CLI prints both.

*Reversal cost:* none. Nothing is removed, so a later decision to retire the
legacy call is still available and would follow the ordinary deprecation path.

### A2 — One struct, not four getters

`amdsmi_get_gpu_cuid_info()` fills a single `amdsmi_cuid_info_t` rather than
offering separate calls for the primary, the derived value, the type and the
auxiliary flag.

The reason is not convenience. The four values must be consistent with each
other: the auxiliary flag describes *that* primary, and a caller that fetches the
derived CUID and then separately asks whether it is auxiliary can be answered
across a seed re-key or a device rescan and record a value with the wrong
provenance. One call, one snapshot.

The primary is `CAP_SYS_ADMIN`-gated at the source, so it is returned as an empty
string to an unprivileged caller rather than failing the whole call. An
unprivileged operator can and should still see the derived CUID; that is what the
derived CUID is for.

### A3 — Report which stage answered

`amdsmi_cuid_info_t` carries the source: driver, daemon/store, or locally
computed. The auxiliary bit already tells a consumer that the identity was
synthesised, but not the converse — a locally computed *canonical* primary is a
real serial read by an unprivileged path, and it is worth strictly less than the
same value published by the driver, because only one of the two is
authoritative.

*Alternative considered:* expose only the auxiliary bit, which the format already
defines, and leave provenance to the caller.

*Rejected because* the caller cannot determine it. Nothing in the value says
where it came from, and the whole staged lookup exists on the premise that where
it came from matters. Reporting it costs one enum field.

### A4 — The seed is write-only through `amd-smi`

`amdsmi_set_cuid_seed()` provisions 32 octets. `amdsmi_get_cuid_seed_info()`
reports whether a seed is provisioned and a non-reversible fingerprint of it, and
never the seed.

The kernel's `cuid_seed` attribute is readable, by design, so that a provisioning
daemon can verify what it wrote — but it is `0600` and `CAP_SYS_ADMIN`-gated, and
it is one device's copy. `amd-smi` is a general-purpose tool that gets run with
`sudo` casually and whose output gets pasted into bug reports. A fleet secret
must not have a path out through it.

The fingerprint is `SHA-256(seed)` truncated to eight octets, which lets an
operator confirm that two nodes carry the same seed — the actual question — with
no way back to the value.

*Reversal cost:* none. Adding a read later is additive; removing one is not.

### A5 — `BUILD_CUID` defaults to on when the library is found

Today it is `OFF` and must be asked for. A feature that must be asked for at
build time is absent from every distributed package, and an identifier that is
absent from the packages is not an identifier.

The option stays, so a build can force it off. The detection is
`find_package(amdcuid CONFIG QUIET)` and a `cmake_dependent_option` whose default
is `ON` when the package is found and `OFF` when it is not — not the
`find_library` the option used to sit behind; see A7 for why the package
replaced it. Where the library is not found the calls compile and return
`AMDSMI_STATUS_NOT_SUPPORTED`, which is the same thing they do today.

*Alternative considered:* hard-require `libamdcuid`.

*Rejected because* it inverts the dependency direction in practice: `amd-smi`
would stop building on any tree that has not built the CUID project, which
includes several downstream integrations that consume `amd-smi` alone.

### A6 — The CLI prints the derived CUID by default

`amd-smi static` shows the derived CUID, the component type, the auxiliary flag,
the source and the node seed's state in its default output — `--cuid` is one of
the subcommand's default arguments, so it appears without being asked for. The
primary appears only under a second, explicit selector, `--cuid-primary`, and
only where the caller could read it; otherwise that field reads
`N/A (requires root)`. The primary embeds a raw serial number; `amd-smi static`
output routinely ends up in public bug reports.

The seed's state rides in the same block rather than in a command of its own.
It is the context the derived CUID printed beside it only means anything in: on
an unprovisioned node every derived CUID is keyed with a public placeholder, and
an operator who has to run a second command to discover that will not run it.

### A7 — A CMake package, and a self-contained archive

`amd-smi` consumes `libamdcuid` through `find_package(amdcuid CONFIG)` and the
imported target `amdcuid::amdcuid`, not through `find_library()` plus a
separately located header directory.

*Rejected the existing approach because* the two halves are found independently
and can disagree: the library from one ROCm install and the header from another
produces a build that compiles against one ABI and links against a different
one, with nothing to say so. An imported target carries its include directory
and its link interface together, so there is one thing to get right.

A consequence that only appeared once the shared SHA-256 landed underneath:
`libamdcuid` is a static library, and a static library's link dependencies
travel with it into the export set even when declared `PRIVATE`. `rocm-sha256`
is a build-tree helper that is never installed, so exporting a link to it
produces a package whose imported target names a target the consumer does not
have — and CMake only reports this at install time, not at build time. The
objects are therefore folded into the archive instead, which is what a consumer
of a static library expects in any case.

*Reversal cost:* none for consumers. Should `rocm-sha256` ever become an
installed, exported component, the fold becomes a link and the imported target
does not change.

## Risks / Trade-offs

- **Two identity calls invite the wrong one being used.** → Mitigated by
  documentation ordering and by the CLI, which labels the legacy value as such.
  The alternative — one call whose meaning changed — is worse by construction.

- **A struct in a published header is an ABI commitment.** → Mitigated by a
  reserved tail sized for the fields already anticipated (node-scoped identities,
  a UnitID accessor), following the pattern the surrounding `amdsmi_*_info_t`
  structs already use.

- **Turning `BUILD_CUID` on by default changes what packages link.** → It links a
  static library with no external dependencies of its own — the OpenSSL
  dependency was removed in the change below this one — so the added surface is
  object code, not a runtime dependency.

- **`amd-smi` gains a way to invalidate every derived CUID on a node.** → That is
  what provisioning is, and it is already true of the kernel's `cuid_seed`. It is
  gated on privilege. The traceability half of the mitigation is only in place
  on the kernel side, which logs the primary-to-derived association at bind and
  on every re-key; neither `libamdcuid` nor `amd-smi` records that association
  today, so a derived CUID issued by the library before a re-key is not
  traceable through this path. Tracked as an outstanding gap against
  `cuid/key-constants`' "Re-keying is an administrative invalidation", not as
  something this change delivers.

## Migration Plan

1. **Header and API**, with the struct and the two seed calls, compiling to
   `NOT_SUPPORTED` stubs when the library is absent. Nothing else depends on
   ordering here.
2. **Rewrite `amdsmi_get_gpu_device_cuid()` as a wrapper**, so there is one
   lookup path from the first commit that has two.
3. **CMake default**, once the calls degrade cleanly.
4. **CLI and Python**, which are the deliverable an operator sees.
5. **Tests**, in `amd-smi`'s own suite. They run with or without CUID support
   compiled in and with or without a GPU: the ABI-shape checks assert
   unconditionally, and the checks that need a device skip rather than fail, so
   the suite is meaningful in CI and stronger on a developer's machine. A fake
   sysfs root was the original plan and is not what landed — nothing in
   `amdsmi_get_gpu_cuid_info()`'s path takes an injectable root, so the
   driver-sourced case is exercised only on hardware.

Rollback: every step is independently revertible; the API additions are additive
and the CMake default is one line.

## Open Questions

- Whether the node-scoped identities — the platform CUID and the CPU CUID — are
  reported by `amd-smi` at all, given that its object model is per-processor.
  They do not fit the handle, and a node-level call is a larger design question
  than this change should settle.
- Whether seed provisioning should also drive the kernel's per-device
  `cuid_seed`, or only the library's node-wide key file. Writing both from one
  call is attractive and would make the two layers agree immediately, but the
  kernel's copy is per-device and non-persistent, so the semantics of "the node
  is provisioned" differ between them. Scoped out until the kernel's node-wide
  seed work lands.
