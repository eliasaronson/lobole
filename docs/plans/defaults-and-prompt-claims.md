# The last literal defaults, and the workflow prompt's claims tested

## The problem

**Creation tools that still open at a literal.** The rule
([tool sessions](../ui/tool-sessions.md#defaults-and-refusals)) is that a
default carrying a unit is derived from what it acts on, and
`src/ui/CreateActions.cpp` says in its header that every default in it is.
These are not:

- **Extrude: 1 mm** (`BeginExtrudeOfSketch`, `src/ui/SketchToolbar.cpp`),
  whatever the profile's size. The most used tool in the app opens as a
  sliver on a 300 mm plate and as a slab on a 0.5 mm one.
- **Datum axis:** through the world origin along X (`CreateDatumAxis`), and
  drawn 6 units long with its label 3 out (`SceneOverlays.cpp`,
  `Overlays.cpp`), while the datum planes and frame triads beside it scale
  with `ScenePlaneHalfExtent`. The datum plane's label also sits at a fixed
  2.6, 2.6 though its quad scales.
- **Pattern spacing:** 3 mm with no source body, and a 3 mm floor
  otherwise; the spacing is 1.5 times the source's X extent even though the
  direction is the only thing that should decide which extent.
- **Rib thickness 1 mm, Pipe radius 2 mm, the helix's radius 3 and pitch 2,
  Thread M8 x 1.25** before a face is picked.
- **Edge Flange 10 mm, Hem 6 mm, Jog 10 mm** when the part has no gauge,
  while Base Flange refuses without one.
- **Fillet 0.5, Shell 0.25, Offset Face 1.0** when there is no body to
  measure.

The `defaults/*` GUI tests check only that each tool changes the document,
so a literal passes them at any scale.

Draft's 5 degrees and Edge Flange's 90 degrees stay: an angle that is the
same on any part is not a scale. Mirror's world YZ plane through the origin
stays: mirroring about the origin is what a symmetric part drawn there wants.

**The workflow prompt's claims.** The MCP `modeling-workflow` prompt
(`WORKFLOW` in `python/lobole/mcp.py`) tells an agent what it can rely on.
Most claims are pinned by a test already. These are not:

- **"Every mutating call returns a `digest`" is false.** `set_variable`,
  the call that most often moves geometry, returns `value`, `scope` and
  `regen` and no digest; digests are attached handler by handler and
  nothing checks that a new one has one.
- **"Leave port 8799 for the human's dev-watch"** is a port written into
  text, the thing CLAUDE.md says goes stale: dev-watch takes any port, and
  the instance registry marks the human's instance (`mode: "gui"`).
- **`render_sheet` and `ui_action` read as tools.** Neither is an MCP tool:
  one is `rpc('render_sheet')`, the other a family of RPCs
  (`menu_click`, `key_press`, ...).
- **Untested:** `set_rollback` showing an intermediate state;
  `check_literals` naming a FRAME on a world plane where a face lies (the
  sketch half is tested); `get_state` carrying `selection` and
  `selection_items`.

## The shape of the fix

### Defaults

Each comes from the measure the tool already has, through the helpers the
blend tools use (`SizeFromBody`, `SizeFromFace`, `TargetBody`, rounded down
to 1, 2 or 5 x 10^n by `RoundDown125`, whose two pasted copies go):

- **Extrude:** the profile's smaller in-plane extent, rounded. A cut on a
  body takes the body's depth along the sketch normal instead, so a cut
  opens through the part rather than a sliver into it.
- **Datum axis:** through the target body's bbox centre (the world origin
  with no body), along X; drawn and labelled at `ScenePlaneHalfExtent`, as
  the datum planes are. The datum plane's label moves with its quad.
- **Pattern:** 1.5 times the source's extent ALONG the pattern direction, no
  floor; with no source body the tool refuses, naming why.
- **Rib:** `SizeFromBody` of the sketch's body. **Pipe:** a twentieth of the
  path's length. **Helix:** radius and pitch from `ScenePlaneHalfExtent`, as
  primitives are placed. **Thread:** if picking the face does not already
  size it to the face's diameter (the nearest standard size that fits), it
  does after this.
- **Edge Flange, Hem, Jog without a gauge:** refused naming the gauge, as
  Base Flange is.
- **Fillet, Shell, Offset Face with nothing to measure:** refused. These
  tools need a body anyway.

### Prompt claims

- `set_variable` returns a `digest` like any other part edit, and every
  method whose `MethodDoc` row can regenerate the part does too: one gate
  (`tests/rpc_digest_gate.py`) drives each such row on a small part and
  fails a mutating response without one. `batch` keeps stripping its
  members' digests and returns one for the whole.
- The port line becomes "never drive the registry's `gui` instance (the
  human's); start your own on a scratch port". `render_sheet` and the GUI
  calls are named as `rpc(...)` calls.
- Tests for the three untested claims, beside the ones that pin the rest.
- `docs/scripting/mcp.md#the-workflow-prompt` gains a table: each claim, the
  test that pins it. A claim with no test is removed from the prompt rather
  than left untested. A new gate (`mcp_smoke`) fails when the prompt names an
  RPC or tool that is not registered.

## Files

- `src/ui/CreateActions.cpp`, `src/ui/SketchToolbar.cpp`,
  `src/ui/SceneOverlays.cpp`, `src/ui/Overlays.cpp`.
- `src/app/guitests/GuiTestsModeling.cpp`: a scale test per tool.
- `src/remote/RemoteProject.cpp` (`set_variable`), and any handler the gate
  finds.
- `python/lobole/mcp.py`; `tests/mcp_smoke.py`; `tests/rpc_digest_gate.py`
  (new); the smokes for `set_rollback`, the frame literal and the selection.
- `docs/ui/tool-sessions.md`, `docs/scripting/mcp.md`, the RPC `describe`
  rows of the new refusals.

## Verification

- What a default must do is stay sensible on the part in front of it: a
  preview that is visible, inside the view fitted to the part, and not a
  sliver. Its exact value matters less. One GUI test opens every creation
  tool on a 2 mm body and a 300 mm one and checks its preview's bbox lies
  within the fitted view and is not under a hundredth of the body's size;
  each refusal checks its reason is shown and the document is unchanged.
  Both fail on main.
- `rpc_digest_gate.py` fails on main (`set_variable`) and passes after.
- The prompt-name gate fails on a planted unknown name.
- The headless suite, the GUI suite, `tools/ctest -L pygui`;
  `python3 benchmarks/runners/reference.py` still scores every check.
