# CUID specification workspace

## Layout

- **`specs/cuid/`** — the **published specification**, as it stands. Written down
  from "Persistent platform component identification for SW tools" (S1) at
  version 84. This is the baseline: it records what the page says, including
  where the page contradicts itself. Those contradictions are marked
  `Recorded contradiction` / `Recorded defect` / `Recorded gap` and are *not*
  resolved here — a baseline that quietly fixed things would stop being a
  baseline.

- **`changes/`** — deltas against that baseline, each with a proposal, a design
  note carrying the reasoning, tasks, and spec deltas.

## The changes, and what each is for

| Change | Layer | State |
|---|---|---|
| `amend-published-cuid-spec` | The published pages | Specified, not applied — needs Confluence write access |
| `add-cuid-kernel-interface` | `amdgpu` driver | Implemented and verified on two W6800s |
| `pin-cuid-cross-layer-contract` | Cross-layer format, keys, vectors | Implemented in both trees |
| `integrate-cuid-into-amdsmi` | `amd-smi` API, CLI, Python | Implemented |

`amend-published-cuid-spec` is the one that has no code. Everything in it is
already implemented by the other three; it exists because the page still
describes something a conforming producer cannot build, and until the page is
edited every new implementation starts from the same three contradictions.

## Why the baseline is recorded verbatim

Three defects reached shipping code because the format was described in prose
and tables and never once in values, and because two tables disagreed about who
owns bit 117. Each was found by hand, months apart, by comparing code to code.

Writing the page down as-is is what made the three contradictions countable
rather than anecdotal:

- bit 117 is claimed by UnitID part 2 in the Primary table and by the Auxiliary
  Value Identifier in the Derived table;
- the derived hash slot is 45 bits wide, labelled 46, inside prose that says
  110;
- the auxiliary input structure gives Format 17 bits and Machine ID 127, so the
  ranges total 256 while neither field is the size its own description needs.

## Conformance vectors

The normative worked examples live in
`changes/pin-cuid-cross-layer-contract/specs/cuid/conformance-vectors/spec.md`
and exist as a generated artifact shared byte-for-byte between the kernel tree
(`tools/testing/selftests/amdgpu/cuid_vectors.txt`) and the library
(`projects/cuid/tests/vectors/cuid_vectors.txt`).

Each tree checks its own copy against the generator, so a hand-edited vector
fails a build: in this tree that is `cuid_vectors.py --check`, run by the
`vectors-drift-check` job in `.github/workflows/cuid-workflow.yml`, and the
library asserts every vector in `cuidtstUnprivileged.ConformanceVectors`. The
generator is the shared definition; the two copies agree because both are its
output, not because either tree can see the other's file.

They are the durable half of this work. A table in a page cannot be checked; a
vector can.
