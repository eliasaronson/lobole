# Workholding — fixtures the verifier can see

## The problem

Nothing in a CAM job knows what holds the stock down. The verifier checks
the cutter against the stock and the part, and the holder against material
it can see. A toe clamp, a vise jaw or a fixture plate is not in the model,
so a path that drives the cutter through a clamp, or a rapid that passes
over the stock at a safe Z lower than the clamp beside it, verifies clean.
Four documents record this, and each works around it separately:

- `reach-and-fidelity-plan.md`: "Fixture and clamp collision stays excluded:
  no fixture solid exists in the model."
- `dogfood-round.md`: checking "the tool actually clears the clamps" is left
  to the operator's eyes.
- `simulation.md`: machine and fixture collision is "a different model".
- `router-round.md`: "Clamps and hold-down keep-outs. Wanted."

It matters most on a first real cut, when a clamp hit is the usual way a job
goes wrong.

Three details of the current code shape the fix:

- **The holder check assumes nothing is taller than the stock.** It exits
  early once the collar rides above the stock top, and it is blind inside the
  cutter's radius (`FoulsHolder`), where the stock is simply carved. A cutter
  driving into a clamp would be recorded as a cut.
- **The verifier's grid covers the stock box only.** A vise beside the stock
  has no cells in it.
- **Safe Z is derived from the stock top only** (`DeriveSafeZ`), so every
  rapid passes through a clamp that stands taller than the stock.

## The shape of the fix

### 1. A fixture is a part, placed on the bed of a setup

A fixture is one of two things:

- **A part.** Any part document, modelled here or imported as a vendor's
  STEP: a vise, a shaped clamp, a fixture plate.
- **A keep-out box.** A size and a place, with no part tab. Most first jobs
  only need "there is a clamp here, this big", and asking for a modelled
  part per toe clamp is the friction that would keep this feature unused.
  A box's default size is derived from the stock.

A setup gains a list of them:

```cpp
struct Fixture {
  uint64_t id;          // from the job's one id counter
  uint64_t sourceDocId; // the part slot it is a copy of; 0 = a box
  glm::dvec3 box;       // a keep-out box's size, corner at its origin
  std::string name;     // display; a part owns its own name
  glm::dvec3 pos;       // setup (bed) space, mm
  double rotZdeg;       // spin about +Z
  bool enabled = true;  // off = drawn, not checked
};
```

The fixture's model coordinates are taken as bed coordinates, Z up, and then
spun and moved: `bed = T(pos) · Rz(rotZ) · model`. That gives two rules:

- **A fixture belongs to a setup.** You re-clamp between setups, so each
  setup holds its own list.
- **It lives in bed space, not model space.** The setup's `frame` turns the
  PART to lie on the bed, and a vise does not turn with it. So a fixture
  never goes through `BodiesInSetupFrame`.

A fixture is not a `JobPart`. A job part is a workpiece: it gets toolpaths,
stock and targets. A fixture is only a keep-out. Keeping them separate is
what stops a clamp from growing the stock box, or becoming a surface the
verifier measures gouges against.

**Where a new fixture goes** is derived from the stock and from the
fixture's own size, never typed:

- **Centre** when the fixture is wider than the stock in X or Y. A vise
  surrounds the stock, and putting it against one side is wrong. Its
  footprint is centred on the stock's, with its bottom at the stock's
  bottom. Where the stock sits in the jaws depends on the parallels, which
  no model states, so the height is the user's to adjust.
- **Otherwise against a side:** on the bed (its bottom at the stock's
  bottom), against that face of the stock, centred along it. The first
  fixture goes on −X, then +X, −Y and +Y.

After that it is moved by number, or re-derived against a chosen side, the
centre, or "top", which lays it on the stock's top face straddling the −X
edge, where a toe clamp's arm belongs.

### 2. The check: a second, static height field

The simulation's one height map stays the one thing that is carved. Fixtures
get a map of their own, never carved:

- **The grid** spans the fixtures' own footprint in the setup (the union of
  their boxes), at the verifier's resolution.
- **Each cell holds the top of the tallest fixture over it**, sampled with
  the same `SampleModel` the designed surface uses, plus which fixture it
  was.
- **On every move, rapid or cut,** each fixture cell the tool's footprint can
  reach is tested:
  - Inside the cutter's radius the tool's underside is `TipHeight`.
  - Out to the collar's radius, when the tool states a holder, it is the
    collar's underside.
  - The tool's height is taken where the segment passes closest to the cell,
    not at the move's lowest Z, so a ramp past a clamp is not reported for a
    depth it never reached there.
- **A hit is a fixture collision.** It is counted per move, with the worst
  depth, where it was (setup space), the G-code line, which fixture, and
  whether the cutter or the holder hit it.

`Verdict` gains `fixtureCollisions`, `worstFixtureDepth`, `worstFixtureAt`,
`worstFixtureLine`, `worstFixtureName` and `worstFixtureByHolder`. `Safe()`
also requires no fixture collisions. `Explain` says it in one line at level
Bad: "3 moves drive the cutter into fixture \"Toe clamp\", worst 4.20 mm at
X12.0 Y-3.0 (line 118)". The stock touching a fixture is the point of a
fixture, and is not a finding.

**When a fixture raises safe Z past the machine's Z travel,** the travel
finding says which fixture did it. Clearing a tall vise makes every rapid
climb, and without the cause named the result reads as a travel problem
with no reason.

**What is not checked is said, not skipped:**

- **A rotary setup.** A setup that emits A, or a file that turns while it
  cuts, is verified in the part's own turning frame, where a bed-space clamp
  has no meaning. The verdict says "Not checked: fixtures, on a rotary
  setup". A chuck and tailstock are a different keep-out, for later.
- **A missing part.** A fixture whose part has been deleted cannot be
  checked, and the verdict names it rather than dropping it.

### 3. Safe Z clears the fixtures

`DeriveSafeZ` takes the tallest fixture top into account, with the same
clearance rule it uses over the stock. Safe Z is already re-derived wherever
the geometry it depends on moves (a new setup, a flip, laying a face up).
Adding, moving or removing a fixture is one more of those. Without this, the
first job with a clamp taller than its stock would report every rapid as a
collision, which is correct and useless.

### 4. What you see and click

- **The CAM panel** gains a **Fixtures** section under Parts, for the active
  setup, in the list-row pattern the consistency round set
  (docs/ui/consistency-round-plan.md), the one CAM tools already use:
  - One row per fixture: a glyph, its name, and where it sits (−X side,
    centred, on top, or placed) as a dim tail. Greyed when it is not
    checked.
  - Right-click gives Edit, Check on/off, a Move-to submenu (−X, +X, −Y,
    +Y, Centre, Top) and Remove.
  - Edit opens the floating tool window with the numbers: position, spin, a
    box's size, and the same placements as buttons.
  - Add buttons, one per part tab the job does not cut, plus "Keep-out box".
  - Every change re-derives safe Z, regenerates and re-verifies, as a
    placement change does.

  The Parts section above it still uses the older inline style. That is its
  own change.
- **The job viewport** draws each fixture translucent and grey, the way a
  Derived reference body is drawn. They are drawn outside the document's body
  list, so picking and selection indices are untouched. Simulate ghosts the
  document's meshes and leaves these alone, so fixtures stay visible while it
  plays.
- **Simulate marks the worst fixture hit** where it happened, from the
  position the verdict already carries.
- **The Verify line** in the panel and the Generate window carries the
  fixture sentence with the others.

### 5. RPC

- `add_job_fixture {setup, part | box, side?, pos?, rot_z?, name?}`:
  exactly one of `part` and `box` (a size in mm). Returns the fixture's id
  and the derived position.
- `set_job_fixture {setup, fixture, pos?, side?, rot_z?, box?, enabled?,
  name?}`: `side` is one of `-x +x -y +y centre top`, and anything else is
  refused, naming those. `pos` and `side` together are refused.
- `remove_job_fixture {setup, fixture}`.
- `list_cam_setups` lists each setup's fixtures.
- `cam_verify` adds `fixture_collisions`, `worst_fixture_depth_mm`,
  `worst_fixture_at`, `worst_fixture_line`, `worst_fixture` (name) and
  `worst_fixture_by` (`cutter` or `holder`), plus a `not_checked` line when
  fixtures were skipped.

## Not in this round

- **Dragging a fixture in the viewport.** It needs fixtures to be pickable,
  which means picking plumbing outside the body list. The numbers and the
  side buttons place a fixture exactly; the drag is convenience.
  **Since v127** a clamp can instead REST on the stock: one of its faces on
  the stock's top, reaching in over an edge. It is placed by two clicks and
  re-solved whenever the stock changes. The bounding-box `top` placement
  stood a step clamp's studs on the work ([fixture-seat-plan.md](fixture-seat-plan.md)).
- **Generated vises and toe clamps.** Real clamps are modelled or imported
  first; generated ones follow if the modelled ones prove tedious.
- **Planning around a fixture.** Operations are told they hit one, not
  steered away. Keep-out regions in the planner come after detection has
  been used on real jobs.
- **The spindle nose.** The tool record has the collar's underside but not
  its height, so there is nowhere yet to hang a nose cylinder. The collar
  check is already unbounded upward (anything inside its radius above its
  underside is a hit), so a nose wider than the collar is the only gap.
- **A setup sheet.** None exists yet. Fixtures are the first thing one would
  carry, but the sheet is its own piece of work.
- **A chuck and tailstock** as the rotary keep-out: the natural next fixture
  type. It would also answer how far past a rotary part's ends the passes may
  go with geometry instead of a guess.
- **Holding the part down on 3D operations.** A separate gap from
  collisions: a whole-part surface pass cuts down to the part's bottom
  around its outline, which frees a part sitting on the stock bottom. Profile
  cutouts have tabs; the 3D strategies have nothing yet.

## Format

`Setup::fixtures`, serialized in the setup as `fixtures: [{id, part | box,
name, pos, rot_z, enabled}]`. It is written only when non-empty, with each key
omitted at its default, so every existing job re-saves byte-identically.

- `kVersion` goes 125 → 126.
- A golden goes in `tests/golden/job/v126-cam-fixture.json` with its
  `.expect.json`.
- A row goes in `docs/versioning/format.md`.

While there: `JobFromBodyJson` rebuilds the id counter from part, setup and
operation ids only, not from placement ids. After a reload, a new placement
could reuse an old placement's id. Fixture ids join the count, and placement
ids do too.

## Files

- `src/core/cam/CamJob.h`: `Fixture`, `Setup::fixtures`, `FindFixture`,
  `FixtureTransform`.
- `src/core/cam/CamVerify.{h,cpp}`: the fixture map, the check, the verdict
  fields, `Explain`.
- `src/core/cam/CamDerive.cpp` and `CamToolpath.h`: `DeriveSafeZ` over
  fixture tops, and the default placement.
- `src/core/io/SerializeCam.cpp`: the setup's fixtures.
  `src/core/io/Serialization.cpp`: the id counter.
  `SerializationInternal.h`: `kVersion`.
- `src/ui/JobActions.{h,cpp}`: `JobFixtureSolids` (a fixture's bodies in bed
  space), and fixtures in `JobRenderVersion`.
- `src/ui/RendererSync.cpp`: the ghosted fixture instances.
- `src/ui/CamPanel.cpp`: the Fixtures section.
- `src/ui/CamTool.cpp`: fixtures into verification, and the one safe-Z
  helper every caller uses.
- `src/remote/RemoteCam.cpp`: the three methods, the setup listing, the
  verify keys.
- Tests: `tests/test_cam_workholding.cpp` (new), `tests/test_golden.cpp`
  with the v126 golden, and a GUI case in `src/app/guitests/GuiTestsCam.cpp`.
- Docs: this plan, then `simulation.md`, `reach-and-fidelity-plan.md` and
  `router-round.md` (the exclusions), `docs/manufacturing-gaps.md` (gap 1),
  and `docs/versioning/format.md`.

## Verification

- **The check** (`tests/test_cam_workholding.cpp`), each against a real
  posted program:
  - A profile whose cutter passes through a clamp beside the stock is
    reported, with the right fixture, line and depth.
  - A clamp the path passes beside, at a clearance, is not reported.
  - A rapid at a safe Z below a clamp's top is reported; with safe Z derived
    over the fixture it is not.
  - A holder that clips a clamp the cutter clears is reported as the holder.
  - A disabled fixture, and a fixture on a rotary setup, are not checked, and
    the verdict says so.
  - A keep-out box is checked exactly as a part of the same shape is.
  - A fixture never changes the stock box, the origin or the toolpath.
- **The default placement** sits on the bed, against the side, clear of the
  stock, and moves with the stock's size. A fixture wider than the stock is
  centred on it.
- **The format:** the v126 golden round-trips fixtures, an old job re-saves
  byte-identically, and a placement created after a reload gets a fresh id.
- **RPC:** a smoke builds a job with a clamp, verifies, and reads the keys; an
  unknown `side` is refused, naming the accepted ones.
- **GUI:** add a keep-out box from the panel, move it with its row's menu,
  and Verify names the collision.
- **Full suites** before committing: headless, GUI, `tools/ci-local.sh --nix`,
  and the pixel gate, because `RendererSync.cpp` changes.
