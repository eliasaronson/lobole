# CAM — 2.5D milling — plan

> Status: **C1–C5 BUILT 2026-08-21**, including the two items this table
> briefly claimed and did not have (ramp entry, frozen G-code goldens). The roadmap's "CAM out of scope" is
> reversed. C5 shipped as an in-house height-map verifier rather than the
> CAMotics dependency this doc originally planned — see "Verification, and
> why not CAMotics".
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
  frame          // work origin + orientation. THE 4-axis seam (see below)
  machine        // post id: "mach3" | "linuxcnc"
  tools[]        // number, diameter, flutes, length, feeds/speeds
  operations[]

cam::Operation
  type      // Profile | Pocket | Face | Drill   (v1)
  targets[] // ElementRef — faces/edges/pockets, stable across edits
  toolIndex
  depths    // top, bottom, stepdown, stock-to-leave
  feeds     // plunge, cut; from the tool, overridable per op
  strategy  // climb/conventional, tabs, ramp/helix entry
```

A toolpath is generated **in setup space** and posted through the setup's
frame. That single indirection is what keeps the 4th axis from being a
rewrite.

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
| **C3** | **Pocket** with islands, **Face**, stepdown + ramp/helix entry | **done** (ramp along the path; a helix for pockets too small to ramp in is the remaining case) |
| **C4** | Post interface + Mach3 implementation, backplot overlay, G-code goldens | **done** (Mach3 + LinuxCNC, backplot, panel, one frozen `.nc` per dialect) |
| **C5** | The closed loop as a ctest — **in-house, no dependency** | **done** |
| later | LinuxCNC post · adaptive clearing (port Tusek) · indexed 4th axis · surfacing (opencamlib) · continuous 4-axis | each on demand |

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
