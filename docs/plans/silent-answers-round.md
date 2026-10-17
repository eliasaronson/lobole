# Answers that were silently wrong

## The problem

The clock and the Strandbeest each passed only after their agents noticed
an answer that said "ok" and was not. Each one breaks the rule that a call
says what it did not do in its own response (CLAUDE.md, "Refuse, don't
substitute").

- **`set_material` on a feature that creates no body answers ok and sets
  nothing.** The clock called it with a cut's id: every body came back with
  `mass_g: null` until the agent passed the creating feature's id. Other
  generators may have the same call, so their masses can be wrong.
- **A modelled thread whose cut fails stays a cosmetic thread, and regen says
  ok.** Only `check_geometry` reports it. The clock's trigger was a pillar
  relief at exactly the thread's root radius.
- **Thread designations parse two ways.** "M8 printed" is accepted and
  "M8 printed nut" is refused as not in the table. `get_print_process` then
  reports the refused thread as "external, Ø0" instead of naming the
  refusal.
- **`print_allowance` says "the built-in tables are community practice…"**
  for a measured profile, whose rows have their own sources.
- **A mesh-mode motion study calls two touching coplanar faces a clash**
  where exact interference says they are clean. The clock had to move its
  wheels apart by an end shake to get past it.
- **Exact interference of modelled threads takes about 6 s a pair and holds
  the main thread** while it runs. The clock's first run spent 12 minutes on
  nut searches.

## The shape of the fix

1. **`set_material`** takes a body's feature, a body, or a part. Given a
   feature that creates no body, it refuses, naming the feature that made
   the body it modifies. Every generator that calls it is checked: each body
   it builds must have a mass after the build, and the counter, the
   planetary, the hook and the jar have their receipts compared before and
   after.
2. **A modelled thread that cannot be cut** is a regen error on its feature,
   named in the response of the call that caused it (as
   `tests/regen_honesty_smoke.py` checks for other features). It is not a
   cosmetic thread. A test builds the clock's case: a relief at the root
   radius.
3. **Thread designations** go through one parser. Every form the docs
   accept is accepted everywhere; anything else is refused with the forms
   that are. A refused thread is never stored, so no report can describe
   it as "Ø0".
4. **`print_allowance`'s confirm text** is the measured source of the row
   that answered, when there is one. The "confirm with a coupon" sentence
   stays for built-in rows only.
5. **Touching is not a clash in a mesh-mode study.** Two triangles that only
   meet in a shared plane, within the mesh's own deflection, are reported as
   `touching` (as exact mode's `seated_contacts`), not as a clash. A test:
   two blocks face to face read touching in mesh mode and clean in exact
   mode, and blocks overlapping by twice the deflection still clash.
6. **Thread interference:**
   - profile it first (`tools/profile.sh`) and fix what the profile shows;
   - every call that can run exact interference on threads is `Deferrable`,
     so it runs off the frame and the watchdog does not see a stall.
   The clock's nut search is the benchmark: before and after times go in
   the report.

## Files

- `src/remote/RemoteModeling.cpp` (or wherever `set_material` lives) and
  its MethodDoc row.
- The thread feature, its regen, and the designation tables
  (`src/core/features/Thread*`, the print process parser).
- `src/core/PrintAllowance.cpp` (the confirm text).
- `src/core/MotionStudy.cpp`, `src/core/Measure*.cpp` (mesh contact).
- Interference: `src/core/Measure*.cpp`, `src/remote/RemoteInspection.cpp`
  flags.
- Tests: the unit tests for each item, `regen_honesty_smoke.py`,
  `refused_calls_smoke.py`, and the generators whose masses change.
- Docs: whatever each change touches (docs/modeling, docs/assemblies,
  docs/tolerances), and the RPC rows.

## Verified by

- a unit test or smoke for each of the six items, failing before the fix;
- the generators that call `set_material` rebuild with a mass on every body;
- the headless, GUI and generators suites;
- the clock's nut search timed before and after.
