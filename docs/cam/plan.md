# CAM — 2.5D milling — plan

> Status: **design, unscheduled** (2026-08-21). The roadmap lists CAM under
> "explicitly not planned"; this doc is the costed proposal to reverse that,
> written after the user bought a CNC mill. It is **not** scheduled work —
> the decision is deliberately left open until the mill has been used enough
> to know what it needs.
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
| **C1** | `cam::Setup` document object: stock, work frame, tool table, panel + RPCs | ~1 session |
| **C2** | Slice → Clipper2 pipeline; **Profile** (with tabs) and **Drill** | 1–2 sessions — *this already cuts real parts* |
| **C3** | **Pocket** with islands, **Face**, stepdown + ramp/helix entry | ~1 session (Clipper2 does the hard part) |
| **C4** | Post interface + Mach3 implementation, backplot overlay, G-code goldens | ~1 session |
| **C5** | The camsim closed loop as a ctest, plus packaging camsim | ~1 session |
| later | LinuxCNC post · adaptive clearing (port Tusek) · indexed 4th axis · surfacing (opencamlib) · continuous 4-axis | each on demand |

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
