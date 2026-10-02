# Design requirements stored with the project

## Problem

What a design is for lives outside it. The pan/tilt head's criteria are
assertions in its generator: lens deflection under 2 mm at 3 g, the camera
clearing the yoke, every bearing seat passing, `check_fasteners` clean. A
person or an agent who edits the model afterwards has nothing that re-checks
them. A check run before an edit looks exactly like one run after it.

Parts of this exist:
- `add_assertion`/`verify_assertions` store a minimum clearance between two
  occurrences with the project;
- every study result is bound to the input hash of the model it measured
  (docs/simulation/README.md#studies-as-jobs).

There is no general requirement, and no single place to ask "does this
design still do what it was for?".

## Fix

**A `Requirement` is a saved object in the project:**
- what it measures, by stable reference;
- a bound (`<=`, `>=`, within a band, or pass/clean);
- a source: why it exists, in a sentence;
- a revision, bumped on every edit to the requirement.

**What it can measure**, each through the RPC that already computes it:
- a study probe: a face's deflection or rotation, peak stress, safety factor;
- a clearance or contact state between two occurrences (today's assertions
  become this kind);
- mass or volume of a part or the assembly;
- a fit's class, or `check_fits`, `check_seat` or `check_fasteners`
  passing;
- `check_literals` clean;
- a motion study clean.

**Checking produces evidence:** the measured value, pass or fail, the input
hash of what was measured, and the requirement's revision. A requirement
shows one of four states:
- **pass** or **fail**: evidence exists for the current model and the
  current revision;
- **stale**: the model changed since the evidence was taken, so the input
  hash differs;
- **unresolved**: its reference no longer exists. This is never read as
  pass.

**Rules:**
- Editing a requirement invalidates its evidence.
- Checking reuses study results by input hash, so re-checking an unchanged
  design is free.
- An unknown measure or bound kind is refused, naming the accepted ones.

**Interfaces:**
- **RPC:** `add_requirement`, `set_requirement`, `remove_requirement`,
  `list_requirements` (with states) and `check_requirements` (a job, since it
  may run studies).
- **GUI:** a Requirements panel with each requirement's state, value and
  bound, and Check.
- **MCP:** the workflow prompt says to state requirements and check them
  before delivering.
- **Generators:** they add requirements instead of asserting in Python. The
  pan/tilt head's become the first real set.

**Saved form:** `requirements` in the project, with evidence beside study
results. This is a format change.

## Files

- a new `src/core/Requirements.*`;
- serialization in `src/core/io/`;
- the RPCs in a new `src/remote/RemoteRequirements.cpp`;
- `src/ui/RequirementsPanel.cpp`;
- `tools/designer_sdk.py`, `tools/designer_mcp.py`;
- `generators/pantilt/`;
- docs: a new README for a `requirements` docs area;
- `docs/versioning/format.md`.

## Verification

- Each measure kind passes and fails on a small model.
- An edit makes the requirement stale, and checking again makes it current.
- A deleted reference reads unresolved.
- Editing the bound invalidates the evidence.
- Re-checking an unchanged design reuses study results (no compute).
- The old assertions load as clearance requirements.
- The pan/tilt head's requirements all pass after a fresh regeneration.
  Raising the camera mass past what the yoke carries makes the deflection
  requirement fail.
