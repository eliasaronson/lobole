# Friction from the PES pump build — plan

Building the Bosch PES4M pump (`generators/pes_pump/`) hit a dozen points
where the API made a script take a detour, and mapping them turned up four
defects already in the tree. This round fixes the ones that are small and
recur; the two that are real modelling gaps are left for their own plan.

## The fixes

### Format (v128): three fields a script could not reach

1. **Revolve and pattern about any axis.** `AxisRef` knows world X/Y/Z,
   a sketch line and a datum axis. A turned bore off the origin needs a
   datum axis or a construction line whose entity id is random, so the
   pump's element bores became stacked extrusions. Add a kind
   `custom {origin: [x,y,z], direction: [x,y,z]}` (mm), resolved by
   `ResolveAxis` like the rest; revolve's own in-plane check still applies.
   Touches `AxisRef.h`, `DatumPlaneFeature.cpp` (ResolveAxis),
   `Feature.cpp` (VisitRefs), `SerializeFeatures.cpp` (JSON + the revolve,
   pattern and datum rows), `RemoteDispatch.cpp` (`axis_kinds`),
   `FeatureEditor.cpp` (the axis combo names a custom axis instead of
   showing nothing), `designer_mcp.py` (`AXIS_REF`, which also lacks
   `datum_axis`).
2. **A helix that starts where it is asked to.** A `Curve3D` helix starts
   on the X direction OpenCASCADE's `gp_Ax3` happens to pick, so putting the
   plunger's control helix on its port meant turning the plunger's mate
   frame 190 deg. Add `start_deg` (+`start_deg_expr`), right-handed about
   the axis from a reference direction that now has a name,
   `HelixReference` — it asks OCCT for the same direction it always used,
   so no existing helix moves — and is written out in the feature's docs
   row. `CurveFeature.{h,cpp}`, serialization.
3. **A motion study that names its configuration.** A study measures
   whatever configuration happens to be active when it runs; the pump's
   camshaft study only measures full load because the generator remembered
   to switch first. Add an optional `configuration` to `MotionStudy` — an
   id, or 0 for the base; absent = whichever is active, as now. The base has
   to be nameable: the pump's rack study must run with nothing held.
   `WhyNot`, the run and the scrub use it.
   `Occurrence.h`, `MotionStudy.cpp`, `MotionActions.cpp`,
   `SerializeAssembly.cpp`, `RemoteMotion.cpp`.

One bump, one row in docs/versioning/format.md, a part golden and an
assembly golden at v128.

### Answers that were incomplete or inconsistent (no format change)

4. **Study samples carry mate values.** With `include_poses`, each sample
   gains `mates: [{mate, name, rotation_deg, offset}]` — the pump's plunger
   turn was recovered from quaternions. Recorded in the sample loop after
   the solve (`MotionStudy.{h,cpp}`), emitted in `ResultJson`.
5. **One clash vocabulary.** `interference` names a pair `a`/`b` (labels);
   `run_motion_study` uses `a`/`b` for ids and `a_name`/`b_name` for
   labels; `contact_report` has labels plus `a_id`/`b_id`. Every report now
   carries `a_name`/`b_name`, and `a_id`/`b_id` wherever the pair is two
   occurrences. `a`/`b` keep their current meaning. Fixes
   `fpv_drone.py`, which reads an `a_id` that never existed.
6. **`set_section` says what it kept.** `reverse` is undocumented and the
   default keeps the half BELOW the plane (negative normal); the response
   gains `keeps` (e.g. `"y <= 0"`), the row explains `reverse`, and the MCP
   wrapper takes `reverse`.
7. **`solve_assembly` documents its answer.** Loop keys (`dof`,
   `loop_mates`, `loops_closed`, `residual_mm`) appear only when there are
   loops — deliberately, `assembly_loops_smoke` pins it. The row says so,
   `Asm.solve()` gets a docstring, and `om615.py` and `toolmakers_clamp.py`,
   which index them unguarded, stop raising KeyError on a tree.
8. **Colours are linear, and the docs say sRGB.** The renderer, STEP and
   GLB all treat `set_appearance` colours as linear; the row and the
   `Appearance` comment say sRGB. Correct the docs (converting would move
   every render and the pixel baseline for no gain).

### The Python layer

9. **`call(method, **params)` swallows a `method` parameter.**
   `app.describe(method="x")` raises TypeError. Make `method` positional-only
   in `designer_remote.call` and `App.call`; `strict_inputs_smoke.py` passes
   it by keyword and is updated.
10. **SDK verbs that were missing or broken:** `SketchBuilder.spline(poles,
    degree=3, closed=False)`; `Asm.rack(rotating_mate, sliding_mate,
    pitch_radius)`; `SketchBuilder.arc()` returns None and attaches pending
    expressions to entity 1 — fix both.

### Dogfood

11. `generators/pes_pump/` uses all of it: element bores revolved about
    custom axes, the helix placed by `start_deg` with an unturned plunger
    frame, `rig.rack`, studies that name their configuration, and the load
    check reading mate values instead of quaternions. It must still measure
    what it measured before.

## Not in this round

- **A relation that reads two inputs** (a delivery valve whose timing
  depends on the rack as well as the camshaft), and **springs** that stretch
  between two occurrences. Both change what the solver is, not how it is
  reached; each needs its own plan.

## Verification

- Unit tests: custom axis (revolve, circular pattern, JSON round trip,
  out-of-plane refusal); helix `start_deg` (start point, expression, round
  trip, unchanged default); study configuration (run under a configuration
  that is not active; WhyNot honours its suppressions); sample mate values.
- Smokes: interference names/ids, `set_section` `keeps`, `describe(method=)`
  through both clients, `sdk_smoke` contracts.
- Goldens at v128; `test_golden` gates.
- `tools/test-fast.sh`, then the full headless suite and the GUI suite
  (the axis combo is UI).
- The pump generator: same strokes, firing order, turns and clean sweeps.
- The doc audit (CLAUDE.md) over every row and page this touches.
