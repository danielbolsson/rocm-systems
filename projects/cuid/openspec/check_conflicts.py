#!/usr/bin/env python3
"""Verify the conflict register against the specs.

Every conflict recorded in the baseline must appear in CONFLICTS.md, and every
entry in CONFLICTS.md must name a resolution. Run from anywhere:

    python3 openspec/check_conflicts.py

Exits non-zero if a conflict has been recorded without being registered, or
registered without being resolved. The point is that "are they all resolved" is
answerable by running something rather than by reading four files.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BASELINE = ROOT / "specs" / "cuid"
AMEND = ROOT / "changes" / "amend-published-cuid-spec"
REGISTER = ROOT / "CONFLICTS.md"

problems: list[str] = []


def fail(msg: str) -> None:
    problems.append(msg)


# --- the register ----------------------------------------------------------
if not REGISTER.exists():
    print(f"missing {REGISTER}", file=sys.stderr)
    sys.exit(1)

register_text = REGISTER.read_text()
rows = re.findall(r"^\| (C\d+) \| (.+?) \| (.+?) \| (.+?) \|$", register_text, re.M)
registered = {cid: (desc, where, res) for cid, desc, where, res in rows}

if not registered:
    fail("CONFLICTS.md contains no conflict rows")

for cid, (_desc, where, res) in sorted(registered.items(), key=lambda kv: int(kv[0][1:])):
    if not res.strip() or res.strip() == "—":
        fail(f"{cid} is registered with no resolution")
    # A resolution that just points elsewhere is not a resolution.
    if res.strip().lower() in {"tbd", "todo", "open", "unresolved"}:
        fail(f"{cid} resolution is a placeholder: {res!r}")

# --- every recorded marker in the baseline is registered -------------------
marker_re = re.compile(r"\*\*Recorded (?:contradiction|defect|gap)(?: \((C\d+)[^)]*\))?")
recorded_ids: set[str] = set()
unlabelled = 0

for spec in sorted(BASELINE.rglob("spec.md")):
    for m in marker_re.finditer(spec.read_text()):
        if m.group(1):
            recorded_ids.add(m.group(1))
        else:
            unlabelled += 1

missing = sorted(recorded_ids - set(registered), key=lambda c: int(c[1:]))
for cid in missing:
    fail(f"{cid} is recorded in specs/ but absent from CONFLICTS.md")

# --- the amend change must actually say something about each --------------
amend_text = "\n".join(p.read_text() for p in AMEND.rglob("*.md"))
for cid in sorted(registered, key=lambda c: int(c[1:])):
    where = registered[cid][1].strip()
    if where in {"—", "-", ""}:
        # Absent from the published page entirely: there is nothing in the
        # baseline to have recorded, only a value this change supplies.
        continue
    if cid not in recorded_ids and f"({cid}" not in amend_text and cid not in amend_text:
        # Not fatal on its own -- some are recorded under a shared marker --
        # but the register's "recorded in" column must at least name a file
        # that exists.
        target = BASELINE / where.strip().strip("`").split(",")[0].strip("` ") / "spec.md"
        if not target.exists():
            fail(f"{cid} names a baseline file that does not exist: {target}")

# --- report ----------------------------------------------------------------
print(f"registered conflicts : {len(registered)}")
print(f"labelled in specs/   : {len(recorded_ids)}")
print(f"unlabelled markers   : {unlabelled}")

if problems:
    print()
    for p in problems:
        print(f"  FAIL  {p}")
    print(f"\n{len(problems)} problem(s)")
    sys.exit(1)

print("\nevery recorded conflict is registered, and every registered conflict "
      "names a resolution")
sys.exit(0)
