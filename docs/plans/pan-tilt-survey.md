# Pan/tilt head: phase 0 survey

What other projects do for the four things docs/plans/pan-tilt-head.md needs:
helical threads in OpenCASCADE, worm pairs, thread milling, and printed
clearances. What to copy, what to avoid, and a recommendation for phases 1
and 2. Copy numbers and formulas, not code: the references are GPL, LGPL,
BSD and Apache, and this repository is AGPL.

The references are local. `~/Documents/CAD/FreeCAD` (PartDesign, CAM),
`~/Documents/CAD/parts/` (freecad.gears, BOSL2, cq_gears, bd_warehouse,
FreeCAD_FastenersWB) and `~/Documents/CAD/slicers/` (PrusaSlicer,
BambuStudio).

## 1. Helical threads in OCC

Four OCC-based implementations exist, and all four sweep one profile along a
helix:

| where | helix | sweep | ends |
|---|---|---|---|
| FreeCAD `PartDesign/App/FeatureHole.cpp` `makeThread` | `TopoShape::makeLongHelix` (`Part/App/TopoShape.cpp`): **one edge per turn**, each a `Geom2d` segment on a `Geom_CylindricalSurface` | `MakePipeShell`, Frenet, `BRepBuilderAPI_Transformed`; end caps from `Simulate(2)`, then sewn and made solid | DIN 76 runout table (`ThreadRunout`, e1 against pitch), thread depth = hole depth − e1 |
| FreeCAD `PartDesign/App/FeatureHelix.cpp` | `makeSpiralHelix(..., breakAtTurn)`, split per turn ("breaking the path at each turn prevents an OCC issue"); left unsplit for a conical helix, where splitting breaks the solid | `MakePipe` Frenet; `ShapeFix_ShapeTolerance::LimitTolerance` on the path, then a user `Tolerance` on the result; `ShapeFix_Solid` | n/a |
| FastenersWB `screw_maker.py` `CreateThreadCutter` / `CreateInnerThreadCutter` / `CreateBlindInnerThreadCutter` | `makeLongHelix`, **2 turns longer** than the thread | `MakePipeShell`, Frenet, RightCorner, `makeSolid` | overlong helix, trimmed by the boolean; blind internal thread `common` with a 118° drill-point revolve |
| freecad.gears `basegear.py` `helical_extrusion` | `makeHelix` | `MakePipeShell` with **`setBiNormalMode(axis)`**: "Frenet tilts LastShape because OCCT's helix 3d-curve is only C1" | n/a |

bd_warehouse `thread.py` goes a different way: it **lofts** each turn
through 11 sections in the axial plane and keeps the loops as a compound.
Its ends are `fade` (the profile scaled to nothing over P/4), `square` or
`chamfer`, and it has an `interference` parameter (0.2 mm default) that
overlaps the thread into the core so the fuse never meets a coincident face.
cq_gears builds the worm's faces directly: `makeSplineApprox` patches for
half a turn, repeated, split by end planes and sewn.

**Failure modes, and the workaround each has in the references:**

1. **Many turns on one edge.** `BRepLib::BuildCurve3d` approximates the whole
   helix as one B-spline, which degrades as turns grow (FreeCAD Mantis
   #0954). The fix is one edge per turn (`makeLongHelix`). Designer's
   `CurveFeature.cpp` helix is one edge today.
2. **Frenet on the approximated helix.** The 3D curve is only C1, so Frenet
   drifts and the last section tilts. Binormal mode,
   `MakePipeShell::SetMode(gp_Dir axis)`, carries the section by the exact
   screw motion: a section in the axial plane stays axial, and a transverse
   one stays transverse. Designer's `SweepProfileAlongSpine`
   (`src/core/features/SweepFeature.cpp`) offers Frenet, CorrectedFrenet and
   Fixed, but not binormal.
3. **Self-intersection between turns.** A profile whose axial height is at
   least P touches the next turn and the solid is invalid. FastenersWB makes
   the cutter 0.95 P tall. FreeCAD's hole pulls p1 and p4 in by
   `marginZ = 0.001` mm. FreeCAD's helix has `safePitch()`, which checks the
   profile's bounding box against the pitch.
4. **Coincident faces with the body.** Take the cutter past the surface it
   cuts. FastenersWB's external cutter reaches r + 3√3/80·P beyond the
   major radius, and bd_warehouse overlaps the core by `interference`.
5. **The ends.** Run the helix past every open face (about one pitch) and let
   the boolean trim it. A blind end is trimmed by a cone (FastenersWB), faded
   (bd_warehouse), or given a runout (FreeCAD, DIN 76).
6. **An inside-out solid.** Both FreeCAD features run
   `BRepClass3d_SolidClassifier::PerformInfinitePoint` and `Reverse()` when
   the result reports IN.
7. **Seam.** Do not start the helix on the target cylinder's seam
   (u = 0). A cutter end face in the seam plane makes the boolean intersect
   along the seam edge. FreeCAD's hole turns its helix 180° about X, partly
   for this reason.

**Exact volume check.** A region in the axial plane (area A, centroid radius
r̄) swept by screw motion removes exactly 2π·r̄·A per turn, because
Pappus–Guldin holds for screw motion. A screw-invariant cutter that passes
through both ends of a length L removes exactly 2π·r̄·A·L/P. Phase 1's test
can therefore assert the volume to a tight tolerance, rather than the "loose
band" in docs/plans/modeled-thread.md.

### Phase 1 recommendation

- **The helix:** one edge per turn, built as today (a `Geom2d_Line` segment on
  the cylinder), plus a partial last turn. It extends one pitch past each open
  end. It does not start at the face's seam angle.
- **The profile:** the ISO 68-1 / UTS basic profile, which is the same for
  metric and UNC, with H = √3/2·P. It is drawn in the axial half-plane as the
  **groove** (the cutter), 0.95 P tall at its wide end, and it overshoots the
  surface it cuts by ≥ P/10. The external groove runs from r = d/2 + 3√3/80·P
  to its bottom at d/2 − 5/8·H (the basic minor), following FastenersWB's
  `CreateThreadCutter`: the flanks sit at ±0.475 P at the outside, and a root
  arc of radius √3/12·P bulges a further half radius. The internal groove runs from inside
  the minor to the major, following FreeCAD's `makeThread`: base 7H/8 below
  Rmaj, crest flat 2·P/16.
- **The sweep:** `MakePipeShell`, `SetMode(gp_Dir(axis))` (binormal),
  `BRepBuilderAPI_Transformed`, `MakeSolid`, then the classifier reversal. Add
  `SweepFrame::Binormal` to `SweepProfileAlongSpine` rather than a
  thread-only path, because the worm needs it as well. Frenet on the per-turn
  helix is the fallback, and it is what FreeCAD and FastenersWB ship.
- **The boolean:** one `BRepAlgoAPI_Cut` of body − cutter, non-destructive
  (the CLAUDE.md rule), `BRepCheck_Analyzer` on the result, and any failure
  kept as the feature's error with the cosmetic overlay still drawn (as the
  modeled-thread plan says).
- **The ends:** square in phase 1. A tapped blind hole is cut by the same
  cutter trimmed by the hole's own drill-point cone. A DIN 76 runout and
  bd_warehouse's fade are later work.
- **Avoid:** fusing turn-loops together (bd_warehouse keeps a compound;
  a cut can take the compound as its tool), and lofted sections (slow and
  approximate where a pipe is exact).

## 2. Worm and wheel

**The worm.** Every library draws the ZA profile: a straight-sided trapezoid
in the **axial** section, which is a thread profile, so phase 1's sweep builds
it. The axial module is m, the axial pitch px = π·m, the lead
pz = z1·px, tanγ = z1·m/d1, the addendum is m and the dedendum is
(1 + c)·m with c = 0.2 to 0.25. freecad.gears (`wormgear.py`) projects that
profile helically into a transverse section and twist-extrudes it with
binormal mode. That is equivalent, and useful when the profile is easier to
state in the transverse plane. cq_gears `worm_gear.py` uses the same rack
points. Its tooth thickness is s0 = m·(π/2 − backlash·tanα)/2.

**The wheel:**

- **freecad.gears / cq_gears:** no wheel. Pair the worm with a helical
  involute gear at helix angle γ. Contact is a point; it is the plan's
  fallback.
- **BOSL2 `gears.scad` `worm_gear()`:** throated without booleans. It takes
  the helical-involute tooth, bends it around an arc of the worm
  (`worm_arc`, default 45°), and shears it by the lead
  (`L = π·d1·w_ang/360·tanγ`). It oversizes the virtual hob by
  `crowning = 0.1` mm "to make them fit the work easier", and needs
  `teeth > 7`. It builds a VNF, not a B-rep, so it cannot be copied, but it
  is a middle fallback: throated, with no boolean loop.
- **Hobbing simulation:** no library here does it. The literature
  (DIN 3975, ISO/TR 14521) defines the wheel as the envelope; the plan is our
  own. The arithmetic behind it:
  - The worm is screw-invariant, so turning it by φ equals translating it
    axially by pz·φ/2π. With the wheel turning θ = φ·z1/z2, the hob's
    position at wheel angle θ is **rotate about the wheel axis by θ, then
    translate along the worm axis by r2·θ** (r2 = z2·m/2; a rack rolling on
    the pitch circle). No sweep is rebuilt per step: every step is a rigid
    transform of one hob solid.
  - The range is the wheel angle over which the hob touches one gap,
    ±(L_hob/2)/r2. For L = 40 and r2 = 40 that is ±0.5 rad, about ±6
    wheel pitches at z2 = 40.
  - The steps: the cusp left between steps is roughly s²/(8ρ), with
    s = π·m/n (n = steps per wheel pitch) and ρ ≈ r2·sinα, the flank's
    radius of curvature. At m = 2, r2 = 40 and n = 8 that is 0.004 mm; at
    n = 4 it is 0.015 mm. **n = 6 to 8 is below print resolution by an order
    of magnitude.** One gap then takes about 12 pitches × 8 = 100 hob
    placements.
  - Cutting one gap and then patterning it z2 times is the order that
    scales. Build one gap envelope G = common(sector wedge around the gap,
    the union of the hob placements). Pass the placements as one
    `BRepAlgoAPI` tool list, not as 100 sequential cuts. Then take
    wheel = blank − {G rotated by i·2π/z2}, again as one multi-tool cut. The
    wedge must be wider than the gap, so that neighbouring G's overlap
    inside air and never share a face.
  - Robustness: rotate the placements by an irrational fraction of the step
    angle, so that no hob face lands exactly on a wedge face. The hob is the
    worm **oversized**, as a real hob is: its tip is larger by 2·c·m for root
    clearance, and its flanks are thickened by BOSL2's `crowning` (about
    0.1 mm). Use non-destructive booleans.
  - Convergence is the measure: the wheel's volume at n against 2n (the
    difference must fall as 1/n²), BRepCheck valid, and the wall time.

**Backlash.** The references thin the worm (cq_gears: s0 reduced by
backlash·tanα) or widen the centre distance (BOSL2 `worm_dist`:
+ backlash·cosγ/tanα). For a printed pair, hob the wheel with the oversized
hob above and print the worm thinned by j/2 per flank, with j = 0.2 to
0.3 mm axial, at the nominal centre distance.

**Printing.** The wheel prints axis-up. The throat's upper half is an
overhang, so keep the throat arc at 90° or less (an overhang of 45° or
less), or split the wheel at mid-face. An upright worm at 20° pressure angle
has flanks 20° off horizontal. Either print it lying down on a flat, or
check it at m ≥ 2, where the overhang is only 2.25·m deep.

### Phase 2 recommendation

The worm is phase 1's sweep with a ZA trapezoid, `starts` rotated copies,
and the binormal frame. The wheel is the hobbing loop above in
`generators/worm.py`: gap envelope, pattern, one cut, measured at
n = 4, 8 and 16. If it does not converge or is too slow, fall back in this
order: BOSL2-style bent tooth (throated, no loop), then a helical involute at
γ.

## 3. Worm pair sizing for this head

**The load:** 1.5 kg with its centre of mass 80 mm up and 40 mm forward of
the tilt axis. At full tilt the lever is √(80² + 40²) = 89.4 mm, so
T2 = 1.32 N·m, or 3.95 N·m at 3 g (handling). Pan carries no gravity torque
on a level tripod; tilt governs.

**Formulas** (axial module, KHK Table 4.23 / DIN 3975, checked on KHK's
example m = 3, z1 = 2, z2 = 30, d1 = 44, giving a = 67, dt = 96, ri = 19):

- d1 = q·m (q, the diameter quotient, about 10), tanγ = z1/q, d2 = z2·m,
  **a = (d1 + d2)/2**
- throat diameter dt = d2 + 2m, **throat radius ri = a − dt/2 = d1/2 − m**,
  wheel tip diameter ≈ dt + m
- worm length b1 ≈ π·m·(4.5 + 0.02·z2)
- **wheel face width b2 ≈ 2m·(0.5 + √(q + 1))**, no more than 0.67·d1
  (AGMA's limit for the effective face)
- efficiency η = tanγ/tan(γ + ρ'), with ρ' = atan μ; **self-locking
  (static) when γ < ρ'**

**Friction.** PETG on PETG, dry: static μ about 0.3 to 0.4 and kinetic 0.25
to 0.35 in the printed-polymer tribology papers (tribology.rs 2024-1 paper
9-1546; ResearchGate 390750034). Take μ = 0.2 as the dry design floor, so ρ'
is 11.3°. A greased pair can fall to μ about 0.1 (ρ' = 5.7°).

| m | d1 (q = 10) | d2 (z2 = 40) | a | γ | Ft at 1 g / 3 g | b2 | Lewis σ at 3 g (Y = 0.3) | tooth at pitch |
|---|---|---|---|---|---|---|---|---|
| 1.25 | 12.5 | 50 | 31.25 | 5.71° | 53 / 158 N | 8.4 | 50 MPa | 1.96 mm |
| 1.5 | 15 | 60 | 37.5 | 5.71° | 44 / 132 N | 10.1 | 29 MPa | 2.36 mm |
| **2** | **20** | **80** | **50** | **5.71°** | 33 / 99 N | **13.4** | **12 MPa** | 3.14 mm |

**Recommendation:** m = 2, z1 = 1, z2 = 40, q = 10, giving d1 = 20, d2 = 80,
a = 50, dt = 84, ri = 8 and b2 ≈ 13. The reasons:

- The 3 g bending stress is about ¼ of PETG's layer-line strength
  (roughly 30 to 50 MPa).
- A tooth 3.1 mm thick at the pitch line is 7 or more 0.4 mm perimeters.
- It self-locks dry with a 2× margin (5.7° against 11.3°). It is **marginal
  if greased**. If the head will be greased, take q = 12 (γ = 4.76°,
  d1 = 24, a = 52).
- It drives with about 0.12 N·m at the worm (η ≈ 0.28 at μ = 0.25), which is
  fine for a knob.

The check reports γ against ρ' at the table's μ floor. That the pair
self-locks under vibration is not claimed: static self-locking is all the
rule gives.

## 4. Thread milling (FreeCAD CAM)

Files: `Mod/CAM/Path/Op/ThreadMilling.py` (the op),
`Path/Base/Generator/threadmilling.py` (the path), `Path/Op/Gui/ThreadMilling.py`
(the fit), and `Data/Threads/*.csv` (the tables).

**The helix radius** (`threadRadii`): internal radius = (d_thread −
d_tool)/2; external radius = (d_thread + d_tool)/2. Here d_tool is the
cutting (body) diameter. With H = 1.6·(Dmaj − Dmin)/2:

- internal: from (Dmin − dt)/2 out to Dmaj/2 + H/8 − crest·k − dt/2
- external: from (Dmaj + dt)/2 in to Dmin/2 − H/4 + crest·k + dt/2

k = 1/(2·tan(θ/2)) = 0.866 for 60°, and "crest" is the tool's tip flat. The
H/8 and H/4 reach the sharp-V tip, so the tool's flat lands at the major (or
minor).

**Passes** (`threadPasses`): constant chip area, with the i-th depth
Hi = H·√((i + 1)/n). `threadPassRadii` (linear steps) is dead code.

**The fit** (GUI): a slider from 0 to 100 %, default 50, interpolating
between the table's min and max for both major and minor.

**The path** (`generate`):

- Move to the "elevator", a point at (Dmin − dt)/2 − 1 mm off centre for an
  internal thread (Dmaj + dt)/2 + 1 for an external one. From there, an
  optional lead-in semicircle out to the start radius, in the thread's own
  direction for an internal thread and the opposite direction for an
  external one.
- Then **half-turn G2/G3 arcs, each advancing Z by P/2**, and a partial last
  arc to the final Z. The partial arc's end point is from k = frac(n), with
  (dx, dy) = (sin kπ, cos kπ)·r.
- The lead-out mirrors the lead-in, back to the elevator. With lead-in/out
  off these are G1 moves. Between passes the tool goes to clearance height.

**Direction** (`threadSetupInternal/External`), right-hand thread:

| | climb | conventional |
|---|---|---|
| internal | G3, bottom → top | G2, top → bottom |
| external | G2, top → bottom | G3, bottom → top |

A left-hand thread swaps G2 and G3 in each cell. The tool is accepted only
if it has `Diameter` and `Crest`. The cutting angle defaults to 60°.

**Copy:** the radius and crest math, the √ pass schedule, the half-turn arc
emission with a partial last arc, the direction table, and the elevator
point. **Avoid:** no check that the tool fits the tap-drill bore beyond a
logged error. Z runs StartDepth → FinalDepth with no overtravel below the last
full thread. The external metric tables are named `4G6G`/`6G`, where the ISO
external classes are lowercase (4g6g, 6g). Designer should say `6g`.

**The tables**, converted from the CSVs (imperial CSVs are in inches; mm below):

| thread | class | major max/min | pitch-dia max/min | minor max/min |
|---|---|---|---|---|
| 1/4-20 UNC, P = 1.270 | 2A | 6.322 / 6.116 | 5.497 / 5.403 | 4.765 / 4.620 |
| | 3A | 6.350 / 6.144 | 5.524 / 5.453 | 4.793 / 4.671 |
| 3/8-16 UNC, P = 1.5875 | 2A | 9.492 / 9.253 | 8.461 / 8.349 | 7.544 / 7.371 |
| | 3A | 9.525 / 9.286 | 8.494 / 8.410 | 7.577 / 7.432 |
| M6×1 | 6g | 5.974 / 5.794 | 5.324 / 5.212 | 4.891 / 4.596 |
| M8×1.25 | 6g | 7.972 / 7.760 | 7.160 / 7.042 | 6.619 / 6.272 |

| thread | class | minor min/max | pitch-dia min/max | major min/max | bore |
|---|---|---|---|---|---|
| 1/4-20 UNC | 2B | 4.978 / 5.258 | 5.524 / 5.710 | 6.350 / 6.629 | #7, 5.105 mm (0.201") |
| | 3B | 4.978 / 5.258 | 5.524 / 5.588 | 6.350 / 6.487 | |
| 3/8-16 UNC | 2B | 7.798 / 8.153 | 8.494 / 8.639 | 9.525 / 9.784 | 5/16", 7.938 mm |
| | 3B | 7.798 / 8.082 | 8.494 / 8.603 | 9.525 / 9.743 | |
| M6×1 | 6H | 4.917 / 5.153 | 5.350 / 5.500 | 6.000 / 6.294 | 5.0 |
| M8×1.25 | 6H | 6.647 / 6.912 | 7.188 / 7.348 | 8.000 / 8.340 | 6.8 |

Other tables present: 4g6g external metric; imperial 2A/2B/3A/3B cover UNC,
UNF and UNEF from 0-80 upward. The sources are listed in `sources.txt`
(amesweb, engineersedge). Note that its internal-metric major max is derived
empirically: dMajorMax = dMajorMin + 1.01·(dMinorMax − dMinorMin). FreeCAD's
hole also carries a per-pitch ISO G allowance (`ThreadClass_ISOmetric_data`:
P 1 → 0.026, 1.25 → 0.028, 1.5 → 0.032 mm on the diameter), which it adds to
a modelled internal thread.

## 5. FDM clearances for PETG

**Slicers:** they compensate but ship no per-material data.

- BambuStudio has `xy_hole_compensation` and `xy_contour_compensation`
  (`libslic3r/PrintConfig.cpp`, applied in `PrintObjectSlice.cpp`). Both
  default to 0; a handful of vendor profiles set 0.02 to 0.025.
- PrusaSlicer has `xy_size_compensation` (the whole object, default 0) and
  `filament_shrinkage_compensation_xy/_z` (0 %, except a few Z values of
  0.18 to 0.33 %).
- The allowance therefore belongs in the model, as phase 4 says, with its
  numbers from calibration or from the references below.

**Threads:**

- FastenersWB `screw_maker.py` `getDia`, "3D printer compatible" mode, with
  the defaults from `Widget3DPrintSettings.ui`: screw
  d' = 0.99·d − 0.05 and nut D' = 1.03·D + 0.10. For M8 that is 7.87 against
  8.34, **0.47 mm diametral in total**. For 1/4-20: 6.24 / 6.64. For 3/8-16:
  9.38 / 9.91.
- BOSL2 `threading.scad`: an internal thread is enlarged by 4·`$slop` on the
  diameter. The examples use `$slop` of 0.05 to 0.15, typically 0.1, which is
  0.4 mm diametral; the default is 0 (`constants.scad`).
- The community range is 0.1 to 0.2 mm radial per side for PETG (0.2 to
  0.4 mm diametral). Printed threads are reliable from M8 up. PETG shrinks
  0.5 to 0.8 %, a little more than PLA.
- Holes print undersize by about 0.2 to 0.3 mm on 10 mm. Part of that is
  faceting: a hole exported as n chords loses the sagitta,
  r·(1 − cos(π/n)). Nophead's polyhole fix is r / cos(π/n). Designer's STL
  export tolerance is part of the allowance.

**Proposed phase-4 table** (PETG, 0.4 mm nozzle, to be confirmed with a
coupon):

- hole +0.2 mm on the diameter below 10 mm, +0.3 mm above
- shaft −0.1 mm
- printed internal thread +0.4 mm and printed external −0.1 mm on the
  diameter
- a metal screw into a printed thread: model the internal thread at the 2B
  maximum + 0.2 mm, or print a plain bore at the tap drill and cut the
  thread with a tap

For the tripod, the 3/8-16 thread in the aluminium plate is milled, so
section 4 applies to it, not this one.

## Sources

Local:

- `~/Documents/CAD/FreeCAD/src/Mod/PartDesign/App/FeatureHole.cpp`,
  `FeatureHelix.cpp`
- `src/Mod/Part/App/TopoShape.cpp` (`makeLongHelix`)
- `src/Mod/CAM/Path/Op/ThreadMilling.py`,
  `Path/Base/Generator/threadmilling.py`, `Data/Threads/`
- `~/Documents/CAD/parts/freecad.gears/freecad/gears/{wormgear,basegear}.py`
- `parts/BOSL2/gears.scad` (`worm`, `worm_gear`, `worm_dist`),
  `threading.scad`
- `parts/cq_gears/cq_gears/worm_gear.py`
- `parts/bd_warehouse/src/bd_warehouse/thread.py`
- `parts/FreeCAD_FastenersWB/screw_maker.py`, `Widget3DPrintSettings.ui`
- `~/Documents/CAD/slicers/{BambuStudio,PrusaSlicer}`

Web:

- KHK gear technical reference, worm dimensions:
  https://khkgears.net/new/gear_knowledge/gear_technical_reference/calculation_gear_dimensions.html
- PETG friction: https://tribology.rs/journals/2024/2024-1/9-1546.pdf and
  https://www.researchgate.net/publication/390750034
- FDM thread clearances:
  https://3dprintbounty.com/blog/3d-printing-tolerances-guide and
  https://www.sovol3d.com/blogs/news/fdm-3d-printing-tolerances-clearances-how-to-design-parts-that-fit
