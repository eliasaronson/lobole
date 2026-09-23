# Verification and the cutting simulation

A wrong FEM number misleads you; wrong G-code breaks a tool, a spindle or a
hand. So the posted file is checked, headless and automatically, and the
picture a person scrubs through is the same simulation the check reads.

Code: `src/core/cam/CamGcode.{h,cpp}` (the parser), `CamVerify.{h,cpp}` (the
height map, the carve, the compare, the verdict and its sentences),
`CamRotary.{h,cpp}` (the rotary twin, [rotary.md](rotary.md)),
`CamFeeds.{h,cpp}` (cutter findings), `CamSim.{h,cpp}` (timeline and
playback stock), `CamStockMesh.{h,cpp}` (meshes), `src/render/StockRenderer`,
`src/ui/CamPlayback.{h,cpp}`, `src/ui/CamTool.cpp` (the panel's verification).

## One engine

- **It runs on the posted file.** `ParseGcode` reads the program back —
  modal state, `F` (a non-positive feed is reported, not adopted), `T`, `M6`,
  `G2`/`G3` as chords, a modal A, `G92 A`, and the `(op N)` comments that
  attribute moves to operations (only exactly `op <digits>`, so a
  `SKIPPED op` line does not start one). `ProgramInSetupSpace` adds the work
  origin back. So a bug in the post, in modal state or in linking is seen,
  and so is a rapid through material that leaves the part perfect.
- **The stock is a height map** `z(x, y)` over the stock box (`MakeStock`),
  and a tool is its profile function `TipHeight(tool, r)` — flat, ball, V and
  drill from one function. `CarveSweep(from, to, tool, stock)` tests each
  cell analytically against the swept segment (exact, not oversampled) and is
  the one carve: `Simulate`, `Inspect` and the playback all call it, carving
  each segment from its own start so a program played in pieces ends
  bit-identical to one played whole.
- **The designed surface** is sampled on the same grid (`SampleModel`, a
  flat-tool drop cutter). It meshes the model detached
  (`GenerateMeshDetached`) at a quarter of a cell, because the default mesh
  deflection scales with the part and would be coarser than the grid
  measuring it, and because meshing a shared shape would re-mesh the
  document behind the renderer's back.
- **One simulator, not two.** The picture comes from the same map, fed by the
  same posted file, as the verdict: `StockSim::SeekTo(End())` equals
  `Inspect`'s machined map bit for bit (a test), so every pixel on screen is a
  number the panel can also state.

## The verifier

`Inspect` (and `VerifyGcode`, which routes a file whose A moves to
`InspectRotary`) compares the machined map with the designed one.

- **Resolution.** `cam_verify` defaults to 0.25 mm cells (`resolution`); the
  panel verifies at 0.4 mm, on a `cam-verify` background job, and its
  tolerance is one and a half cells — a "gouge" of half a cell is
  quantisation.
- **Neighbour rule.** A cell is gouged only if it is below everything its
  neighbours were designed to be, and left over only if above all of them, so
  a cell centre landing on an island's corner does not read as the island's
  whole height, and two grids of different resolution agree.
- **Leftover only where the tool went** (`Inspection::touched`): a job that
  clears one pocket in a block leaves the rest of the block correctly. A
  program that machines nothing at all shows as `untouchedFraction`.
- **Below the stock** (`below_stock_mm`) is cutting into the spoilboard,
  which is what a through cut is for; it is reported and never counted as
  gouge.
- **Told, not guessed:** each toolpath's corner reliefs are skipped for gouge,
  and its tabs are excused for leftover up to their own height (a relief
  excuses a cut too deep, a tab one that did not happen, so a tab needs its
  height or it would excuse a genuinely uncut slab above it).
- **Rapids through material** are counted with the worst depth, place and
  line (`rapids_through_stock`, `worst_rapid_depth_mm/_at/_line`: a graze and
  a crash are the same count and different numbers). The rapid check models
  the tool by `TipHeight` like the cut does, so a drill retracting up its own
  hole does not find its own cone in the way. A rapid never carves.
- **File errors and warnings.** What the parser could not read (a NaN
  coordinate, a zero feed) counts against `ok`; what a controller may refuse
  though the parser survived it (a nested comment) is a warning.

### Verdict

`ok` (`Verdict::Safe`) means nothing scraps the part or breaks the tool: no
gouge beyond tolerance, no rapid through material, nothing past the travel,
no holder or fixture collision, and a file that read straight — the flag to
gate on. `complete` means nothing is left standing. They are separate
because a height map cannot watch a slug drop out of a through cut, so a
correct job can be `ok` and not `complete`; tabs do not count against
`complete`. `cam_verify` also reports how much is left over and where
(`leftover_area_mm2`, `leftover_regions`, `leftover_biggest`, largest first):
several tiny regions are inside corners a round cutter cannot reach and are
correct, one large region is a missing operation.

`Explain` turns a verdict into the sentences the panel shows and `cam_verify`
returns as `says`, each at level Good, Warn or Bad; `MachineLines` adds the
work-area lines ([setup.md](setup.md#the-machines-work-area)). The verdict
text is asserted in tests against jobs with known faults, so the words cannot
drift from the numbers.

### Holder collisions

A tool that states both `overall_length` (tip to the collet nut's underside)
and `collar_diameter` (the nut at its widest) has its holder checked: on every
move, a cell standing above `tip + overall_length`, further from the axis than
the cutter's radius and no further than the collar's, is the nut in the work
(`FoulsHolder`, `holder_collisions` with worst depth, place and line). Blind
inside the cutter's radius on purpose: there the material is either cut or it
is a cut deeper than the flute, which the reach finding already states. Silent
unless both numbers are stated, and the RPC refuses one without the other —
half a holder would disable the check while looking like it enabled it. A
collar riding above the uncut stock top is skipped whole, which keeps the
check nearly free on ordinary jobs. The tool glyph draws the stated collar.

### Fixture collisions

Fixtures ([setup.md](setup.md#workholding-fixtures)) get a height field of
their own, never carved: over the union of their footprints, each cell holds
the top of the tallest fixture there (sampled with `SampleModel`) and which one
it is. On every move, rapid or cut, each fixture cell the tool can reach is
tested — against `TipHeight` inside the cutter's radius and against the
collar's underside out to the collar's radius when the tool states a holder —
with the tool's height taken where the segment passes closest to the cell, so
a ramp past a clamp is not charged for a depth it never reached there. A hit
is reported per move: `fixture_collisions`, `worst_fixture_depth_mm/_at/_line`,
`worst_fixture` (name) and `worst_fixture_by` (`cutter` | `holder`);
`fixtures_checked` counts them. The stock touching a fixture is the point of a
fixture and is not a finding. What is not checked is said: a fixture whose
part is gone, and every fixture on a rotary setup (`not_checked`). When safe Z
was raised to clear a fixture and then exceeds the Z travel, the travel line
names the fixture.

### Machine travel

With a work area stated, `outsideTravel` counts moves past it: X and Y against
the reach window ([setup.md](setup.md#the-machines-work-area)); Z as a span,
the program's deepest point against its highest retract.

### Findings about the cutter

`cam::FeedsFor` (`CamFeeds.cpp`) says what can be said about each tool's feeds
and reach — with the arithmetic and its source beside each sentence — and
what could NOT be checked (`unchecked`), because "checked, fine" and "nobody
looked" are different answers. Feeds stay the user's; these are findings, not
prescriptions.

- **Chip load** = feed / (rpm × flutes), against a band per material class
  (from the material of the bodies being cut) scaled by the cutter's
  diameter, or the tool's own `chipload_min`/`chipload_max` when stated (a
  vendor chart for that cutter beats any rule). Under the band is said, not
  shouted: light machines often run under it and still cut. When the band
  needs a feed the machine's `rapid_feed` cannot reach, that is stated too.
- **Reach**: the deepest cut against `flute_length`, with a margin for the
  collet, per operation (a micron of overrun, not a nanometre); skipped for
  `surface_rotary`, whose Z span is not a depth.
- **Chip room** for full-width cuts (the `FullWidth` column: profile, drill).

The findings reach the panel's verdict and `cam_verify`.

## The simulation

### Timeline

`BuildTimeline(program, rapidFeed, model)` turns the parsed file into a
cursor space: the cursor is `(segment, fraction)` and the slider axis is
**path distance**, not a clock. Cutting time can be defended from each
move's own feed; a rapid traverse rate is a fact the setup usually does not
state, and inventing one would put a fabricated cycle time in front of a
user. Time is a readout (`cutting_minutes`, rapids included only when a rate
is stated; a turning cut clocked the controller's way). The timeline holds the
segments that begin an operation or a tool change, for the skip buttons.

### Stock

`StockSim` holds the pristine block and the cursor. Forward is incremental;
**backward re-runs from the pristine block**, because a height map is lossy
and a lowered cell cannot say what it was — a full re-run of a real program is
a frame. The rotary twin keeps snapshots ([rotary.md](rotary.md#the-rotary-picture)).

The height map is meshed by runs, not cells (`BuildStockMesh`): equal-height
cells merge along a row and identical rows merge, a vertical wall joins
differing heights (a milled step is a wall), and a skirt closes the block. The
mesh encloses exactly the volume the map describes, including a cut below the
stock bottom; a milled block meshes to far fewer triangles than cells (gated).
The stock remeshes whole, at most 12 times a second while playing.

`StockRenderer` draws it from host-visible, persistently mapped vertex and
index buffers per frame in flight (the `FieldRenderer` pattern): no device
wait except when a buffer grows. It honours the section clip plane, depth-tests
against the scene and is registered in `RegisterSceneRenderers` like every
scene renderer. It takes the scene's light directions, colours and ambient from
`core/Conventions.h`, the same constants the mesh renderer reads.

### Tool glyph

The tool is a `MeshRenderer` instance — static geometry that moves, built once
per tool number and placed by transform each frame. Its mesh revolves the
`TipHeight` profile (so it shares its shape with the removal math), with the
flute at full length, a short collar, and the stated holder collar when there
is one. Normals are per segment, blended only across joints within a crease
angle, so a flat shoulder shades flat and a ball's tip stays round; every
triangle winds the way its normals point (gated for all four shapes and for
the stock mesh). The material is matte: a mirror finish slides reflections
across the glyph as the camera orbits.

### Transport

**Simulate** (`job.simulate`) opens a floating transport along the bottom of
the viewport (`DrawCamTransportWindow`): start, back one, play/pause, forward
one, end, next operation, next tool change (only when the job has one), the
scrub bar, speed, a Path toggle and Close, and a readout of the operation, the
tool, the G-code line and the feed. It refuses in words, at the click, when
nothing is generated. Playback:

- ghosts the model bodies at 0.22 (the part inside the block is most of why
  the picture is worth having), re-applied every frame because instance ids
  change on every regeneration;
- draws the path already cut dim and the path ahead bright (ahead of the
  cursor it is usually buried in uncut stock and appears as the cutter
  arrives);
- marks the worst fixture hit; leaves fixtures visible;
- never regenerates: it reads the cached toolpaths and the posted text, and a
  model edit invalidates it rather than re-cutting under the cursor;
- is a session, not a mode, with one exit (`EndCamPlayback`) that restores
  everything — a GUI test asserts nothing is left behind;
- wakes the frame pacer while playing;
- uses a steady clock, not ImGui's, so an offscreen instance can play.

The overlay colours that land on the workpiece (`kDoneColor`, `kAheadCut`,
`kGhostLine`) are calibrated against the stock's brightness; they are the
ones to re-check whenever stock or part shading changes.

`cam_simulate {setup?, at?: 0..1 | "end", play?, close?, resolution?}`
generates toolpaths if they are not current, leaves the app showing the
simulation (screenshot it, or `close`), frames the job once as it opens, and
returns `distance_mm`, `total_mm`, `cutting_minutes`, `removed_mm3`, the
operation, tool, feed, line and position — so a headless caller can assert a
simulation did something without owning a renderer.

## Tests

`tests/test_cam_verify.cpp` and `test_cam_sim.cpp` (headless):
`SeekTo(End())` equals `Inspect`'s map; scrubbing past and back lands on the
same map; the mesh is the map (bounds, volume, below-stock cuts, the triangle
budget); the distance axis is monotonic and ends at the total; the parser keeps
feed, tool change and operation; a rapid is not counted as time nobody
stated; a zeroed-on-corner job plays its own verdict. The GUI cases
`cam/simulation_plays_and_gives_it_back`, `cam/simulate_says_why_at_the_click`
and `cam/a_rotary_part_turns_as_it_plays` run windowed, because an offscreen
instance has no panels and a renderer drawing into the wrong sub-region looks
right there. `tests/render_pixel_smoke.py` covers the renderers.
