# CAM — 2.5D milling — plan

> Status: **C1–C5 BUILT 2026-08-21**, including the two items this table
> briefly claimed and did not have (ramp entry, frozen G-code goldens), and
> reached through the verbs of [gui.md](gui.md) rather than a parameter
> editor. The roadmap's "CAM out of scope" is
> reversed. C5 shipped as an in-house height-map verifier rather than the
> CAMotics dependency this doc originally planned — see "Verification, and
> why not CAMotics". **C6–C9 BUILT 2026-08-21** — helix entry, arcs, adaptive
> clearing and an indexed 4th axis, see
> [Phase 2](#phase-2--what-v1-deferred-designed). A **cutting simulation**
> built on C5's height map is BUILT — see [simulation.md](simulation.md).
> **A setup is no longer part of the part**: since v73 it lives in a JOB
> document that references parts and says how they are cut, which is what
> makes a job about more than one of them — see
> [job-document.md](job-document.md). Everything below still describes the
> generator, the post and the verifier accurately; only the object that owns
> a setup moved.
>
> Scope decided at writing: **2.5D first**, with the data model shaped so a
> **4th (rotary) axis** is an addition rather than a redesign. Continuous
> 4-axis is deferred with reasons. Machine: **Mach3 today, LinuxCNC planned**.

## Why this is even considered

The same argument that reversed FEA: it was "explicitly not planned" until
its real prerequisites — toponaming, materials, headless testing — had
shipped, at which point it became a workstream rather than a wish. CAM's
prerequisites are now in place too, and one of them is unusual:

**Machining selections survive model edits.** "Machine this pocket, these
holes, that face" is stored as an `ElementRef` — the same stable name that
survives a regeneration. Every export-to-another-CAM workflow loses this: you
change a fillet and your CAM job is orphaned. That is the differentiator, not
toolpath quality, and it is *already built*.

The second is the agent story: an operation is a document object edited over
the same command layer as everything else, so "add a 6 mm pocket op on that
face, 2 mm stepdown" is a scripted call, not a GUI ritual.

## What already exists here, and what a survey found elsewhere

Surveyed 2026-08-21 by cloning the projects to `~/Documents/CAD/CAM/` and
reading them, not by reputation.

### The alternatives, measured

| Project | Size | Verdict |
|---|---|---|
| **FreeCAD CAM** (was Path) | ~25k lines of Python ops (Surface 2571, SurfaceSupport 2747, Waterline 2469, Adaptive 1997, Slot 1707, Profile 1489, Pocket 898…) over **libarea, 14 349 lines of C++**, plus ~30 posts | Parity is a decade of contributions. Not a target. Remains the escape hatch: export STEP, machine there |
| **Kiri:Moto** | Whole CAM mode ≈ 12.4k lines of JS *including* 3D topo, animation and export. Individual ops: **pocket 47, drill 77, outline 94, level 95, rough 135 lines** | The load-bearing lesson: **an op is cheap once the infrastructure exists.** The cost is offsetting, slicing, linking, and the post |
| **CAMotics** | `camsim <input.gcode> <output.stl>` — headless simulate-and-write-surface. GPL, SCons + its own cbang library, not packaged in nixpkgs | Not a CAM. A **verification oracle** (see below), and the one real packaging cost |

### The pieces worth taking

Designer is **AGPL-3.0**, so LGPL and GPL code are both compatible to use —
which is what makes the two hardest algorithms available rather than
research.

| Library | Licence | Role |
|---|---|---|
| **Clipper2** (`clipper2-2.0.1`, already in nixpkgs) | Boost | Polygon clipping + offsetting. `ClipperOffset` with Miter/Round/Square joins and `EndType::Polygon`. This is the 2.5D primitive, and it costs nothing to adopt |
| **`libarea/Adaptive.cpp`** (FreeCAD) | LGPL-2.1-or-later | Kresimir Tusek's adaptive clearing, 4.6k lines, clean `Adaptive2d` interface (`toolDiameter`, `stepOverFactor`, `stockToLeave`, `helixRamp*`, `Execute(stockPaths, paths, clearedPaths…)`), built on Clipper 1. Adaptive is the one algorithm nobody should reinvent |
| **opencamlib** | LGPL-2.1+ | 8k lines; drop-cutter and waterline against **triangle meshes**, which we already produce. Boost coupling is one `foreach` include. Not in nixpkgs → vendor as a submodule *if* 3-axis surfacing is ever wanted |

### What Designer already owns

- **The slicer.** `Measure::SectionCaps(bodies, origin, normal, trimPlanes)`
  already returns each body's planar cross-section at a plane. That is
  exactly the Z-level step a 2.5D CAM needs — take the wires, hand them to
  Clipper2. Written, tested, and used by the section tool.
- **Stable selections** — toponaming `ElementRef`, as above.
- **The document-object pattern** — `fem::Study` is a doc-level object with
  its own serialization, panel and RPC surface. `cam::Setup` is that shape.
- **`StlAPI_Reader`** (`CADKernel.cpp`) and the **`compare_geometry`** RPC —
  the two halves of the verification loop below.
- Line overlays for backplot, materials for feeds/speeds, headless RPC and
  golden-file tests for G-code.

## Verification, and why not CAMotics (decided 2026-08-21)

The plan below proposed shelling out to CAMotics' `camsim`. Building it means
packaging **two SCons projects** (CAMotics + its own cbang library), neither
in nixpkgs — a session of work to acquire a GPL binary, plus permanent
maintenance of someone else's build. So the projects were read instead
(`~/Documents/CAD/CAM`), and the survey changed the decision:

- **FreeCAD's PathSimulator is a height map** — `Array2D<float>` plus a tool
  *profile function* (radius at a height, which is how ball/bull/V tools work
  generically) and `ApplyLinearTool`/`ApplyCircularTool`. ~1 500 lines
  including display tessellation.
- **CAMotics is a full implicit-field simulator** (`CutWorkpiece :
  FieldFunction`, AABB trees of tool sweeps, marching cubes). General enough
  for undercuts — and that generality is exactly why it needs its own build
  system. We are 3-axis; a height map is the correct model.
- **opencamlib's drop-cutter and PyCAM's DropCutter** are the same idea from
  the other side: sample a mesh's height on a grid.

So C5 is ~350 lines with **no dependency**: parse the posted G-code, sweep the
tool through a height map, sample the model's surface on the same grid,
report **max gouge** and **max leftover**. One deliberate improvement on
FreeCAD's version: it walks the path in 0.6-pixel steps "to make sure all
pixels in the path are visited", while this tests each cell analytically
against the swept segment — for a check that has to be trustworthy, exact
beats "probably hit every pixel".

The decisive property is that it runs on the **posted file**, not the
in-memory toolpath. A bug in the post, in modal state, or in linking is
invisible to anything inspecting `std::vector<Move>` — and one of those (the
`G0 Xnan Ynan Z20` retract) had already shipped. It also catches what a shape
comparison cannot: **a rapid through material**, where the part comes out
perfect and the tool does not.

Honest caveat: this is our code checking our code, so a shared misconception
could agree with itself. Going through the file removes most of that overlap
(only the geometry libraries are shared). If CAMotics is ever installed, an
optional cross-check that runs when `camsim` is on `PATH` — and skips
otherwise — is the right way to add a second opinion without a dependency.

## The verification loop, which is the reason to be comfortable with this

A wrong FEM number misleads you. Wrong G-code breaks a tool, a spindle, or a
hand. So verification is designed in first, and it can be **closed and
headless** using pieces that already exist:

```
generate G-code → camsim → machined surface (STL) → import → compare_geometry
                                                    against the designed body
```

…asserted in ctest, with a tolerance. "The G-code we emit machines the part
we designed" becomes a test rather than a hope. Nothing else in this plan is
allowed to ship before this loop does — it is C5 only because it needs
something to verify, not because it is optional.

Alongside it, in the app: a **backplot** overlay (rapids in one colour, feed
moves in another, drawn from the same toolpath the post consumes — never a
re-derivation, or the picture and the file can disagree).

## Data model

```
cam::Setup                     // a document object, like fem::Study
  name, stockRef | stock box (derived from the model's bbox + margins)
  frame          // origin + orientation. THE seam — see C9, and
                 // docs/cam/setup-frame.md, which split the orientation into
                 // a FIXTURE frame and the machine's rotationA after C9
                 // shipped the two as one number
  machine        // post id: "mach3" | "linuxcnc"
  fitArcs        // post curves as G2/G3 (C7). A machine property, so it
                 // lives here and persists
  tools[]        // number, diameter, TYPE (flat|ball|vbit|drill) + tip angle,
                 // flutes, length, feeds/speeds. The type is what the
                 // verifier simulates and what an edge break reads (R15).
  operations[]

cam::Operation
  type      // Profile | Pocket | Face | Drill | Adaptive | EdgeRound
  targets[] // ElementRef — faces/edges/pockets, stable across edits
  toolIndex
  depths    // top, bottom, stepdown, stock-to-leave
  feeds     // plunge, cut; from the tool, overridable per op
  strategy  // climb/conventional, tabs, ramp/helix entry
```

A toolpath is generated **in setup space** and posted through the setup's
frame. That single indirection is what keeps the 4th axis from being a
rewrite — and C9 proved it, at the cost of one lesson: *everything else* has
to agree about which space it is in. The engine entry points that take a
`Setup` now frame their bodies themselves, and the viewport transforms back
for display (`cam::FromSetupSpace`), because leaving that to callers meant
the verifier compared a rotated program against an unrotated part and
reported a 25 mm gouge that was not there.

## Leaving the 4th axis open (deferred, not designed away)

The user plans a rotary axis. Two very different things wear that name:

- **Indexed 4th axis** — rotate the work to a discrete A angle, then run
  ordinary 2.5D. This is most hobby 4-axis use, and in this model it is
  *already almost free*: a second `Setup` whose `frame` carries a rotation
  about X, and a post that emits one `A` word before the moves. No new
  toolpath maths. **The design commits to this shape now** (frame-per-setup,
  toolpaths generated in setup space, post takes the frame) and implements it
  when the hardware exists.
- **Continuous 4-axis** (wrapping toolpaths around the axis, simultaneous
  A+XZ motion) — a different toolpath engine. FreeCAD needs `RotarySurface`
  (726 lines) plus five rotary generators (`rotary_dropcutter`,
  `rotary_parallel`, `rotary_rings`, `rotary_spiral`, `rotary_wrap` — 1 434
  lines together) to do it. **Explicitly deferred**; it also wants the
  drop-cutter machinery from opencamlib, so it is naturally downstream of
  3-axis surfacing.

The one thing v1 must not do is assume three axes in the *post interface* or
in the toolpath representation — a move carries an optional A, even if
nothing emits one yet.

## Post-processors: one interface, Mach3 first, LinuxCNC next

The machine is Mach3-based today and LinuxCNC later, so two dialects are
certain and the post is an interface from day one — with the FreeCAD sizes as
the sanity check on scope: **mach3_mach4_post.py is 150 lines, linuxcnc_post
is 481, grbl_post is 191.** A post is small; a post *framework* is what gets
expensive, so this stays a narrow C++ interface (`EmitMove`, `EmitToolChange`,
`Preamble`, `Postamble`) with one implementation per machine and golden-file
tests per dialect.

Conventions to settle in C4, because they are where posts differ: work
offsets (G54…), tool change (Mach3 M6 with a manual-change pause vs LinuxCNC's
tool table), arc output (see the risk below), coolant, and safe-Z retract.

## Phases

| | scope | size |
|---|---|---|
| **C1** | `cam::Setup` document object: stock, work frame, tool table, panel + RPCs | **done** |
| **C2** | Slice → Clipper2 pipeline; **Profile** (with tabs) and **Drill** | **done** |
| **C3** | **Pocket** with islands, **Face**, stepdown + ramp/helix entry | **done** (ramp along the path; the helix for pockets too small to ramp around is C6 below) |
| **C4** | Post interface + Mach3 implementation, backplot overlay, G-code goldens | **done** (Mach3 + LinuxCNC, backplot, panel, one frozen `.nc` per dialect) |
| **C5** | The closed loop as a ctest — **in-house, no dependency** | **done** |
| **C6** | Helix entry for pockets too small to ramp around | **done** `b00cbf9` |
| **C7** | Arcs (`G2`/`G3`) in the posted file | **done** `6c73990` (flat arcs; helical deferred) |
| **C8** | Adaptive clearing (vendor Tusek's `Adaptive2d`) | **done** `27033f2` (LGPL, linked as a shared library) |
| **C9** | Indexed 4th axis (one A per setup) | **done** `ee530f9` |
| **S1-S5** | Cutting simulation: scrub the posted program, watch the stock come off ([simulation.md](simulation.md)) | **done** 2026-08-27 |
| later | 3-axis surfacing (in-house Z-map, or vendor opencamlib) · continuous 4-axis · round stock | each on demand, and none of them costed |

## What C1–C4 shipped

`cam::Setup` is a document object beside `fem::Study`, deliberately outside
the regen chain hash (asserted): describing how to cut a model must never
rebuild it. Setups serialize inline in both formats (**v58** + golden pair)
because a setup is parameters, not a mesh. Nine RPCs in `RemoteCam.cpp`, a
`CAM` dock tab with per-operation verdicts, and a backplot fed from the SAME
`std::vector<Toolpath>` the post consumes — so the picture cannot disagree
with the file.

Four operations: **profile** (outside/inside, tabs), **pocket** (voids inside
the outline; islands fall out of the boolean), **face**, **drill** (targets
by stable element name). Two posts, Mach3 and LinuxCNC, differing in exactly
three places: preamble, tool change, program end.

Three bugs the tests and the first real run caught, all of the kind this
workstream exists to prevent:

1. **Tabs stepped over the part.** Testing "is the END of this segment inside
   a tab?" misses every tab on a profiled rectangle, because that path is
   four 40 mm segments. Tabs now split the segments they cross, with an
   explicit vertical move at the boundary (square walls, not a ramp).
2. **`G0 Xnan Ynan Z20`** in the posted file — the retract before the first
   tool change passed modal X/Y that nothing had set yet. Retract touches Z
   alone now, and a test asserts no posted file ever contains `nan`.
3. The final depth pass lands **exactly** on the requested depth (short,
   never deep), which is a one-line rule and a scrapped part when wrong.

## Entry ramps and frozen G-code (the two items the table had over-ticked)

**Ramp entry.** Loop entries descend along the toolpath at 3°, instead of
plunging straight down — a vertical plunge with a non-centre-cutting end mill
rubs rather than cuts. The tool rapids to the height the previous pass
already cut (air, by construction), then ramps; the lap then starts where the
ramp finished, which is why the point list is rotated rather than restarted.
A loop shorter than the ramp descends over exactly one lap — steeper than
asked, bounded, and still not a plunge. Drilling still plunges, being a
plunge by definition. A **helix** for pockets too small to ramp around is the
one case left.

Two things this surfaced, both worth knowing:

- The verifier immediately reported *a rapid through material* on a test job
  whose `topZ` was the part top rather than the stock top — with 1 mm of
  margin above the part, the entry rapid drops into it. A true finding about
  the job, and an argument for the panel's default to derive `topZ` from the
  stock rather than the model. **Done** with the GUI round: both entry points
  (the verbs and the panel's `Add operation`) now call one
  `cam::DeriveOperation`, so there is a single place a depth comes from.
- The reported cut length is ~0.4% above twice the perimeter, because the
  ramp ends mid-segment and the lap restarts there. The test tolerance says
  so rather than hiding it.

**Frozen G-code.** `tests/golden/cam-block-{mach3,linuxcnc}.nc`, compared
byte-for-byte; substring checks prove the pieces are present, only a golden
proves the file stopped changing. `DESIGNER_REFRESH_CAM_GOLDEN=1` regenerates
them, and the diff is meant to be read before it is committed — every line of
those files eventually moves a machine.

## Making it usable

The engine was done while reaching it was still a parameter editor. The
redesign — verbs on a Tool-window session, a picked top face, derived depths
and a visualisation ladder — is [gui.md](gui.md), and it **shipped**
(G1–G4, 2026-08-21). Two clicks make a pocket, nothing is typed, and Generate
verifies without being asked.

Two things it changed here, because they were engine bugs rather than
presentation:

- `Setup::safeZ` is an absolute Z that defaulted to 5 mm — inside any stock
  taller than that. `cam::DeriveSafeZ` puts it above the stock top, and the
  verifier is what noticed (a rapid through material on the very first job
  made by clicking).
- Leftover is measured only over the cells the tool actually swept
  (`Inspection::touched`). Measuring it over the whole stock made every
  partial job report the unmachined remainder as a failure, which is the
  fastest way to teach someone to ignore a verdict.

## Phase 2 — what v1 deferred, designed

> Status: **BUILT** (2026-08-21), C6-C9. Written as a design after costing
> the four against the code and against the clones in `~/Documents/CAD/CAM`
> — which changed one estimate materially (see C8) — then built in that
> order. What each phase says below is what shipped; the
> [as-built notes](#as-built-notes) at the end record where reality argued
> back.
>
> **Cut a real part first.** Every number below assumes the current output is
> correct in the ways a simulation can check, and the GUI round found three
> defects that only a screenshot caught. The first piece of wood will reorder
> this list better than any estimate.

Excluded on purpose: **3-axis surfacing**, because it breaks the 2.5D framing
(stepover semantics, the Setup model and the verb all need rethinking) and
because it is the item least aligned with cutting plates on a hobby mill.
When it is wanted, the in-house route is cheaper than it looks —
`CamVerify::SampleModel` is already a flat-tool drop cutter on a grid, and
what it lacks is a tool-profile dilation — but opencamlib (14 322 lines, not
in nixpkgs) computes exact cutter-location points against triangles and is
the better answer if accuracy ever matters more than the dependency.

Order matters between these four: **C6 → C7 → C8 → C9**. C6 helps
immediately and is half a session. C7 makes everything after it post
compactly. C8 brings its own helix entry, so doing it before C6 would leave
the concentric pocket and the profile — which keep their ramp — still
plunging steeply on small loops. C9 is independent; do it when the machine
has the axis.

### C6 — Helix entry (~½ session)

Today `EmitLoop` clamps the ramp to one lap: `rampLen = min(descend /
tan(3°), perimeter)`. A loop shorter than the ramp descends over exactly one
lap — bounded and not a plunge, but on a 10 mm pocket with a 6 mm cutter that
lap is short and the descent is steep. The fix is to descend on a **helix
inside the region** instead of on the ring.

The region's largest inscribed circle comes nearly free: `GeneratePocket`
already offsets inward until the result is empty, and a region shrinks to
nothing exactly when the offset exceeds its inscribed radius — so the last
non-empty `inset` brackets that radius to within one step-over, and the
centroid of that innermost ring is where the helix goes. Enough to size a
helix; a bisection refines it if it ever matters. This is a hint computed in
the loop that already runs, passed into `EmitLoop`, not a new geometric pass.

- Helix radius = inscribed radius − tool radius, i.e. the circle the tool
  *centre* can walk. When that is not positive the pocket is barely wider
  than the cutter: keep today's one-lap descent, which is correct behaviour
  rather than a fallback to apologise for.
- Segment count from the same chord tolerance the section uses (0.02 mm), not
  a literal count — the defaults rule.
- The helix is emitted at the **start** of the depth pass, then the rings;
  the lead-out from the helix to the first ring is a cut at depth, so it must
  stay inside the cleared circle.
- `Drill` still plunges, being a plunge by definition. `Profile` keeps the
  along-the-path ramp: there is no inside to helix into.
- Once C7 lands, the post can fit the helix into real helical arcs. Until
  then it is a polyline, which is correct and verbose.

*Verification:* a pocket small enough that the ramp does not fit descends by
helix and emits no `Plunge`; every helix point sits at least a tool radius
inside the wall (the check that catches a helix radius computed from the wall
instead of the tool centre); the descent angle never exceeds 3°; `MinZ` is
still exact.

### C7 — Arcs, `G2`/`G3` (~1 session)

**Fit at post time, not in the toolpath.** The backplot draws `Move` lists
and the verifier consumes the posted file; keeping `Toolpath` a polyline
means the picture, the height-map simulator and every existing test are
untouched, and only two places learn about arcs. A `Move::Kind::Arc` would
instead reach serialization, the backplot, the sweep in `Simulate` and the
goldens.

- **Where:** a pass in `CamPost.cpp` over the move list before emission,
  recognising runs of points that fit a circle, emitting `G2`/`G3 X Y I J`.
- **Tolerance:** the same 0.02 mm chord deflection the section used to
  produce the polyline. That is the point — the polyline is already an
  approximation of a real arc at that tolerance, so a fit tighter than it
  emits arcs only where an arc actually was, and never invents one across a
  corner.
- **`I`/`J` are incremental, and the mode word goes in the preamble.**
  LinuxCNC defaults to incremental centres; Mach3 is settable (`G90.1` /
  `G91.1`). Emitting `I`/`J` without stating the mode puts the arc centre at
  the machine origin on a machine configured the other way — a full-rapid
  arc across the table. This is the one genuinely dangerous line in the phase,
  so the preamble states it explicitly per dialect rather than relying on a
  default.
- **`G17` only** (XY plane), which the preamble already emits.
- **Flat arcs in v1.** A helical arc (an arc with a `Z` word) is legal in
  both dialects and is a one-line extension afterwards, but its risk profile
  is different enough to want the flat case proven on the machine first.
- **The verifier learns to parse them** and converts each arc to chords at
  parse time, so `Simulate` is untouched and the check stays exact for what
  it sees.
- Both goldens regenerate. The diff gets read before it is committed.

*Verification:* a circular pocket posts as arcs rather than hundreds of `G1`s
(line count drops, `G2|G3` appears). The strongest test available: post the
same job twice, once with fitting off and once on, and require the verifier's
gouge and leftover to agree within tolerance — a bad fit shows up as a
geometry difference rather than as a file that merely looks tidier. Plus a
rejection test: a deliberately loose tolerance must not produce an arc across
a corner.

### C8 — Adaptive clearing (~1–1½ sessions)

The survey said "port Tusek's `Adaptive2d`, 4.6k lines, built on Clipper 1".
Reading it changed the estimate: **the public interface is Clipper-free.**
`Execute(stockPaths, paths, clearedPaths, progressFn)` takes `DPaths` —
`vector<vector<pair<double,double>>>` in millimetres — and returns
`AdaptiveOutput` whose `AdaptivePaths` are `(MotionType, DPath)` pairs, also
in millimetres. Clipper1 is an internal detail. So this is vendoring and
bridging, not porting.

- **Vendor** `deps/libarea/`: `Adaptive.{cpp,hpp}` (4 618 + 291 lines,
  LGPL-2.1-or-later, compatible with this AGPL-3.0 app) and libarea's
  Clipper1 (`clipper.{cpp,hpp}`, 5 177 lines, BSL-1.0) because Adaptive still
  uses it internally. Both licences go in the packaging doc. **Pin the commit
  we copy from and record it** — FreeCAD's header currently includes *both*
  `clipper.hpp` and `clipper2/clipper.h`, so this file is mid-migration and
  "we copied it from FreeCAD" will not be enough to reproduce later.
- **Bridge** ≈ 60 lines: `cam::Loop` ↔ `DPaths`, both already in millimetres.
- **`OpType::Adaptive`** is a new serialized value ("adaptive"), so it is a
  format change: **`kVersion` 59 plus a golden pair**, per the standing rule.
- **Parameters map straight across:** `stepOverFactor` from `stepover`,
  `stockToLeave`, `tolerance` from the chord deflection,
  `helixRampTargetDiameter`/`MinDiameter` from the tool, `opType` from
  `Side`.
- **The output mapping is the real work.** Three motion types:
  `mtCutting` → `Cut` at feed; `mtLinkClear` → a move at depth through
  already-cleared material; `mtLinkNotClear` → retract to safe Z, rapid,
  re-enter. Getting the third one wrong drags the cutter through solid
  material at feed — which is exactly what the verifier's
  rapid-through-material check catches, so the test already exists.
- **Seven failure flags** (`StartPointNotFound`, `LeadPathFailed`,
  `UnclearedAreaRemains`, …) become `Toolpath::error` strings in words. A
  boolean would be a regression against the refusal discipline the rest of
  the workstream holds.
- **When it is used:** the verb stays one verb. Adaptive is chosen
  automatically for pockets deeper than one tool diameter — the regime where
  low radial engagement at full depth pays, and where a small spindle
  benefits most — with the Advanced disclosure able to force either
  strategy. "Two clicks, nothing typed" survives.
- **Runtime:** `Execute` takes a progress callback and can be slow on a large
  region. v1 runs it synchronously and states the region cap; if it is slow
  in practice the app already has the `TopoJob`/`StudyJob` async pattern to
  move it onto.

*Verification:* an adaptive pocket and a concentric pocket of the same
region leave the same material (compare through the verifier's height map,
not by comparing move lists); no link move crosses uncleared material
(`rapidsThroughStock == 0`); the radial engagement never exceeds the
requested step-over, measurable as the cleared-area increment per move; and
a golden `.nc`, because this is another file that eventually moves a machine.

### C9 — Indexed 4th axis (~1–1½ sessions)

The data model was shaped for this and the shape held: `Setup::rotationA`
exists, `Move` carries an optional `A`, and the post already emits an `A`
word when a move has one. Nothing has ever set them. What is missing is the
frame, the pick, and the safety rule.

1. **`SetupFrame(setup)`** → a transform. Slicing runs on the bodies
   transformed by its inverse, so "up" in the rotated setup is +Z again and
   the entire existing pipeline — `SectionLoops`, `PickedFaceRegion`,
   `StockBounds`, the verifier — works unchanged. One
   `BRepBuilderAPI_Transform` per setup, cached beside its toolpaths. This is
   the whole reason the plan insisted toolpaths be generated in setup space.
2. **"Rotate to this face"**: click a face, solve for the A that puts its
   normal on +Z. Refuse when there is no solution — a face pointing along ±X
   can never face the spindle under an A-only rotation, and saying so is more
   useful than rotating to the nearest wrong thing.
3. **Posting several setups into one file.** `PostProcess` takes one setup
   today; it takes a list, with a retract, the `A` move, and an optional
   `M0` between them. **The safety rule: a rotation is only ever emitted with
   the tool at safe Z, and the post asserts that rather than trusting its
   caller.** A rotary move with the tool down is the 4th-axis equivalent of
   the rapid-through-stock bug, and it deserves the same treatment.
4. **Verification per setup frame**: the height map is Z-up, so each setup is
   checked in its own frame with the model transformed the same way —
   `Inspect` needs no change. The strongest test: a part rotated 90° in the
   model, machined by a setup at A=0, must produce the same toolpath as the
   unrotated part machined by a setup at A=90°.
5. **Stock stays a box** in v1 — an indexed job on a trunnion is usually
   plate work. Round stock about the A axis is deferred with the rest of the
   rotary work.

Explicitly not this phase: **continuous 4-axis** (simultaneous XYZA). It
needs a different toolpath representation, because feed along a rotary axis
is not a linear distance, and a different verifier, because a planar height
map cannot represent a wrapped surface. It is a workstream, not a phase.

### As-built notes

**C6.** As designed. The inscribed radius does come free from the offsets the
pocket already iterates.

**C7.** Two bugs worth keeping in mind for any future fitting. A circle
through three ADJACENT tessellation points is ill-conditioned — it put a
3 mm corner arc 0.014 mm off centre and split each corner in two — so the fit
is least-squares over the whole run, refitted as the run grows. And checking
deviation only AT the sample points let a 1.5 mm straight edge be swallowed
into a 10 mm-radius arc that passed within 5 µm at every sample and bulged
27 µm in between; each chord's bulge is now bounded too. The centre is slid
onto the perpendicular bisector of the endpoints so the two radii match
exactly, which is what a controller checks before it refuses an arc.

**C8.** The estimate held: reading the interface first is what made it
vendoring rather than porting. `USINGZ` turned out to be required — Adaptive
uses Clipper2's Z callback to track which input edge produced each output
vertex — and it changes `Point64`'s layout, so it has to be PUBLIC on the
target or two translation units disagree about a struct they pass to each
other. The library narrates itself to `std::cout`, swallowed at the call site
so the vendored copy stays byte-identical.

**C9.** Also as designed, and the cheapest of the four to *build* — the
frame transform is the only new idea. But it was the most expensive to get
right, because "toolpaths are in setup space" had been true and unexercised
since C1: at `rotationA = 0` setup space and model space are identical, so
nothing had ever depended on the distinction. Indexed, the verifier, the
backplot, the ghost, the depth label, the stock box and the derived depths
were each in the wrong frame. Fixed by moving the framing INSIDE the engine
entry points that take a `Setup`, so a caller cannot forget. The safe-Z rule for the index is enforced in the post.

**But only half the shape landed, and the half that did not took nine months
to surface.** "Toolpaths in setup space" shipped; the *frame* did not. C9
stored one `double rotationA` where the plan above says `frame`, so how a
part is CLAMPED and what angle a TRUNNION moves to became the same number —
and choosing an orientation silently chose a machine. A part modelled
upright (any panel imported from STEP or Onshape in its assembly
orientation) could only be machined by indexing a rotary axis, which then
posted `G0 A90` at 3-axis routers. The split is
**docs/cam/setup-frame.md** (S1–S7, v67): `frame` for the fixture,
`rotationA` for the machine, and `hasRotaryAxis` so a shop without the axis
can never be handed one. Everything else in C9 survived intact — the framing
really was inside the entry points, so the fix was one function deep.

**A trap that cost two test runs, and would cost a user more.** A GUI test
that fails partway never reaches its own cleanup. When C7 changed the
preamble, a stale `G90 G21` assertion in the CAM panel test failed, so the
test never handed the shared dock tab back to the model tree — and an
unrelated tree test two positions later failed with "unable to hover Model".
Cleanup that matters to other tests belongs immediately after the last
interaction that needs it, not at the end of the test.

## Turning it off

`-DENABLE_CAM=OFF` and the workstream does not exist in the build: no sources,
no Clipper2, no RPCs in `describe`, no CAM tab, no tests. The suite passes
either way, and `cmake` prints which features are in. The dependency it
governs is one vendored library with no transitive deps — but a flag is
cheaper than trusting that stays true.

## Risks

- **Integer space.** Clipper2 works in scaled integers. One mm↔int scale
  convention, chosen once and documented, or tolerance bugs appear at the
  third decimal and nowhere else.
- **Arcs.** Offsetting flattens arcs to polylines. v1 emitting only `G1` is
  correct and safe but produces large files and slightly faceted curves;
  arc fitting on output (libarea has `m_fit_arcs`) is the known later fix.
- **Feeds and speeds are not ours to invent.** The tool table holds what the
  user actually runs. The app computes nothing it cannot justify.
- **Offset robustness on real profiles** — islands, tiny fillets, corners
  that self-intersect at depth. Mitigated by Clipper2 (rather than OCC's
  `MakeOffset`, whose quirks `RibFeature` already documents: it misbehaves
  off the XY plane and silently fails on single-edge wires) and by a corpus
  of nasty profiles as a test suite.
- **Scope gravity.** Every CAM project drifts toward "one more strategy". The
  discipline here: v1 is four operations for one machine, and the escape
  hatch (FreeCAD CAM via STEP) stays documented so the drift has somewhere
  else to go.

## Verification

- Headless: toolpath geometry against known shapes (a rectangle profiled with
  a 6 mm tool is an offset rectangle of a known perimeter); G-code goldens per
  post; the camsim closed loop above.
- GUI: backplot matches the emitted file; an op with a vanished target refuses
  visibly (the section-tool rule — refuse, never guess).
- Real: cut something on the mill. No amount of simulation replaces the first
  part, which is why C2 is scoped to reach a cuttable profile early.
