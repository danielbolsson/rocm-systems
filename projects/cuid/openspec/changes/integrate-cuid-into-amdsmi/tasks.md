Ordering matters only between group 1 and the rest: everything else consumes the
struct that group 1 defines.

## 1. Public API

- [x] 1.1 Add `amdsmi_cuid_info_t` with the primary string, the derived string,
      the component type, the auxiliary flag, the source enum, and a reserved
      tail sized like the neighbouring `amdsmi_*_info_t` structs.
- [x] 1.2 Add `amdsmi_cuid_source_t` (`DRIVER`, `STORE`, `LIBRARY`, `UNKNOWN`)
      and `amdsmi_cuid_component_type_t` on the on-wire values.
- [x] 1.3 Add `amdsmi_get_gpu_cuid_info()`.
- [x] 1.4 Add `amdsmi_set_cuid_seed()` taking exactly 32 octets, privileged.
- [x] 1.5 Add `amdsmi_get_cuid_seed_info()` returning provisioned/default and the
      8-octet fingerprint, and nothing else.
- [x] 1.6 Document the legacy device UUID call as superseded, stating that it is
      not a CUID and that it changes on repartition.

## 2. Implementation

- [x] 2.1 Implement `amdsmi_get_gpu_cuid_info()` over
      `amdcuid_get_handle_by_bdf()` plus `amdcuid_query_device_property()` for
      the primary, the derived value, the device type and the temporary flag.
- [x] 2.2 Return an empty primary rather than failing when the primary query
      reports a permission error.
- [ ] 2.3 Determine the source: driver where the device's sysfs CUID attribute
      exists, store where the record file answered, library otherwise. **Only
      the driver half is implemented.** The store and library stages are not
      observable from `amd-smi`: `libamdcuid` writes a value it computed back
      into the store it consults, so an entry for a device says only that
      something has looked it up before, and which stage answered is a fact
      about the inside of one library call that the library does not report.
      The code guessed it from `access("/tmp/cuid", R_OK)` -- one global file,
      for every device, and the unprivileged one at that -- and so reported
      `STORE` for every GPU on any machine where that file existed, including
      one whose value it had just computed. `cuid_source_for()` now reports
      `DRIVER` or `UNKNOWN` and nothing else. Completing this needs a
      `libamdcuid` change: a query that reports the stage that answered.
- [x] 2.4 Rewrite `amdsmi_get_gpu_device_cuid()` as a wrapper over 2.1.
- [x] 2.5 Implement `amdsmi_set_cuid_seed()` over `amdcuid_set_hash_key()`,
      propagating the store failure.
- [x] 2.6 Implement the fingerprint as the first 8 octets of unkeyed SHA-256 of
      the seed in use, computed inside the CUID library so the seed does not
      cross the ABI.
- [x] 2.7 Stub every entry point to `AMDSMI_STATUS_NOT_SUPPORTED` when built
      without the library, so the ABI is the same either way.

## 3. Build

- [x] 3.1 Default `BUILD_CUID` to on when `find_package(amdcuid CONFIG)` locates
      the exported package, off when it does not, and keep the explicit
      override. (`find_library` was the original plan; A7 replaced it with the
      CMake package. `cmake_dependent_option` supplied the default until it was
      found to defeat the override: when its condition is false it sets a
      *normal* variable to the force value, shadowing the cache entry the user
      set, so `-DBUILD_CUID=ON` on a tree with no `libamdcuid` left
      `BUILD_CUID` false in every subsequent `if()` — the `FATAL_ERROR` for
      that case could not fire, and an explicit request silently produced a
      CUID-less build. A plain `option()` defaulted from `amdcuid_FOUND` keeps
      the auto-detected default and leaves the explicit request visible; it
      also puts `BUILD_CUID` in the cache as a `BOOL`, which is what lets
      `tests/amd_smi_test` see it for 5.8.)
- [x] 3.2 Confirm the static library links with no new external dependency, and
      that the exported target does not reference the uninstalled `rocm-sha256`
      (A7).
- [x] 3.3 Confirm a tree with no `libamdcuid` still builds and links.

## 4. CLI and Python

- [x] 4.1 Add the derived CUID, component type, auxiliary flag and source to
      `amd-smi static`.
- [x] 4.2 Mark the fields unsupported rather than omitting them when there is no
      value.
- [x] 4.3 Gate the primary behind an explicit request and a privilege check.
- [x] 4.4 Add the seed provisioning command, reading from a file or stdin only.
- [x] 4.5 Report the seed's provisioned state and fingerprint. Folded into the
      `--cuid` block of `amd-smi static` rather than given its own command: it
      is the context the derived CUID printed beside it only means anything in,
      and an operator who has to run a second command to learn that the node is
      unprovisioned will not run it.
- [x] 4.6 Carry every field through JSON and CSV output under stable names.
- [x] 4.7 Expose the same calls through the Python interface.

## 5. Tests

_`tests/amd_smi_test/unit/gpu/cuid_info_test.cc`, ten cases, and
`tests/python/unit/gpu/test_cli_cuid_seed.py`, nine. Run against two W6800s with
the CUID driver loaded, in a build with `libamdcuid` and in one without, and
under both an ordinary user and root. Everything that needs hardware, the CUID
driver, or a particular identity skips rather than fails, so the suite is
meaningful in CI and stronger on a developer's machine._

_Nothing here provisions a seed. A real provisioning re-keys every derived CUID
on the node and needs root; a test that did it would be destructive and would
only run for half the people who ran the suite._

- [x] 5.1 Snapshot call against a fake sysfs root: driver-published value is
      returned verbatim and reported as driver-sourced
      (`CuidSourceIsDriverWhenTheAttributeIsPublished`). The two halves are
      checked separately because they belong to different layers. The
      driver-sourced half fabricates `cuid_secondary` for each GPU's BDF under
      a temporary root and points `AMDSMI_CUID_SYSFS_ROOT` at it — the override
      the source fix added — so it runs on any machine with a GPU rather than
      only where the CUID driver is loaded, and it also asserts the negative:
      with nothing published, the source is not reported as the driver. The
      verbatim half compares the returned derived CUID against the real
      attribute's contents, which is only checkable where the attribute is
      real, and skips where it is not. That amd-smi hands back whatever
      `libamdcuid` read is the property being checked; that `libamdcuid` reads
      the driver's value first is the CUID project's own contract.
- [x] 5.2 Unprivileged path does not fail and returns a populated derived value;
      the primary is asserted against the identity the suite is running under
      (`CuidSnapshotIsSelfConsistent`). A non-root run asserts the primary *is*
      empty, a root run asserts it is populated and well-formed. The weaker
      "empty or well-formed" form accepted both answers from both callers,
      which would have passed a snapshot that handed an unprivileged process
      the serial-bearing primary. Each run asserts the branch it is in, so one
      run is enough; between CI and a developer's sudo run both get exercised.
- [x] 5.3 Auxiliary flag agrees with payload bit 117 of the returned derived
      CUID (`CuidSnapshotIsSelfConsistent`), and the decoder that check relies
      on is pinned against all thirteen published conformance vectors
      (`CuidAuxiliaryBitDecoderMatchesConformanceVectors`, needs neither a GPU
      nor `libamdcuid`). The assertion read the wrong octet — bit 7 of rendered
      octet 14, a VendorID bit, where the framing puts payload bit 117 in bit 7
      of rendered octet **15**. Against the vectors the old expression
      disagrees with the payload on A-1, A-2 and D-2: wrong on both auxiliary
      vectors, right on ordinary ones by luck, which is why it passed.
- [x] 5.4 The single-string call and the snapshot call return the same string
      (`CuidSingleStringCallMatchesSnapshot`).
- [x] 5.5 A 16-octet and a 64-octet seed are both refused, with no state change
      (`TestCuidSeedLengthIsEnforced`, `TestCuidSeedLengthEnforcedInTheBinding`
      in `tests/python/unit/gpu/test_cli_cuid_seed.py`). Tested at the two
      layers that enforce it and not at the C entry point, which takes a
      fixed-width array and has no length to check. The CLI cases cover a file
      and stdin, and assert the error names the file and its byte count; both
      layers assert the refused seed never reached the library. A 32-octet
      control is included, without which a command that refused everything
      would also pass. The binding cases skip where no `amdsmi` package new
      enough to have the call is importable.
- [x] 5.6 The fingerprint of the canonical fallback seed is stable and matches
      the value the library computes
      (`CuidSeedFingerprintMatchesTheCanonicalFallbackSeed`). The expected
      value, `be8937fba7ed4e6f`, is the first 8 octets of the unkeyed SHA-256 of
      the 24-octet public seed `AMD-CUID-DEFAULT-SEED-v1`, pinned as a literal
      rather than recomputed, so a producer that fingerprinted the wrong bytes
      fails instead of agreeing with itself. Both branches are asserted: an
      unprovisioned node must report exactly that value, a provisioned one must
      not.
- [x] 5.7 No seed octet appears in any output stream after provisioning
      (`TestCuidSeedNeverReachesOutput`). Drives the real CLI provisioning
      command with a distinctive 32-octet seed and the library call stubbed
      out, then asserts that neither the whole seed nor any 8-octet window of
      it appears — as raw bytes or as hex — in stdout, in stderr, or in
      anything the command published to the logger. That last one is the
      complete input to every renderer, so no format can print what is not in
      it; the positive half asserts the command reports the fingerprint and the
      provisioned state, and no third key. Scope: provisioning is simulated,
      not real, for the reason given above, so this covers everything `amd-smi`
      emits and not what the kernel or `libamdcuid` might log during an actual
      re-key.
- [x] 5.8 A build without `libamdcuid` returns not-supported from every entry
      point (`CuidEntryPointsNotSupportedWithoutTheLibrary`, plus the
      `NOT_SUPPORTED` assertions the seed cases now make instead of skipping).
      The cases could not tell a CUID-less build from a device the library has
      nothing to say about — that the ABI is identical is the whole point — so
      they treated `NOT_SUPPORTED` as a reason to skip and a build without the
      library asserted nothing. `tests/amd_smi_test/CMakeLists.txt` now passes
      `BUILD_CUID` to the test target, which makes the distinction available at
      compile time; with valid arguments, all four entry points must report
      not-supported.
