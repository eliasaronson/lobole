# The friction round's tail

Things the pan/tilt friction round (docs/plans/pantilt-friction-round.md)
found and left. Each is small, but each costs a user something today.

## 1. Study results saved with the project

**Problem.** Static, modal and motion results are reused only within a
session. Topology results are saved with their input hash
(docs/simulation/README.md#studies-as-jobs) and reused after a reopen; these
three are recomputed after every reopen. For the pan/tilt head that is
minutes for an exact motion study.

**Fix.** Save each result next to the topology result, under
`studies/<id>-<slug>.result.json` or a sibling, with its `input_hash`. Use
the same reuse and stale rules as topology. Keep only what the panel and the
RPCs read, never the solver's scratch. This is a format change.

## 2. Hole patterns carry their copies' marks

**Problem.** A Hole writes a `HoleMark` (drill point, thread depth) and a
`ThreadMark` on its body, but a pattern of holes doesn't give its copies
those marks. CAM and the verifier therefore treat a patterned tapped hole as
a plain bore.

**Fix.** The pattern copies the marks, transformed with each copy.

## 3. CAM reads distance tolerances

**Problem.** Size tolerances aim a finish pass (docs/cam/operations.md#tolerance-bands),
but distance tolerances, such as a shoulder-to-seat length, are ignored.

**Fix.** A floor or a shoulder whose distance from a reference face carries
a tolerance is cut to the middle of that band. The verifier judges the cut
against the band, and the posted header states it.

## 4. A suite run cannot be relinked under

**Problem.** `tools/build.sh` and `tools/merge-branch.sh` hold `build/`'s
lock, but a plain `ctest` doesn't. A merge can relink `designer_tests` in
the middle of a suite run, so tests fail once and pass on rerun.

**Fix.** A `tools/ctest` wrapper takes a shared lock on `build/`; builds and
merges take it exclusively. Every documented test command and script uses
the wrapper.

## 5. The roadmap says what is built

**Problem.** `docs/roadmap.md` still lists modelled threads, imperial sizes
and thread milling as unbuilt.

**Fix.** Audit it against the docs, and remove or reword every stale line.

## Files

- **1:** `src/core/fem/*` and `src/core/MotionStudy.*` serialization, and
  `docs/versioning/format.md`.
- **2:** `src/core/features/PatternFeature.*`.
- **3:** `src/core/cam/*`.
- **4:** `tools/`, `CLAUDE.md` and `docs/`.
- **5:** `docs/roadmap.md`.

## Verification

- **1:** each study kind is reused after a save and reopen, and reads stale
  after an input change.
- **2:** a patterned tapped hole is threaded and listed by CAM like a single
  one.
- **3:** a shoulder with a distance tolerance is cut to the band's middle
  and judged against it.
- **4:** a merge waits for a running suite.
- **5:** the `[docs]` test, and a read-through.
