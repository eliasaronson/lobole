# A pin support, and a peak that sits on a support's edge

## The problem

A static study has one support, `fixed`: every node of the face held in all
three directions. A part hung on a pin is not held like that. Clamping a
bore glues it all round, so the half the load pulls away from is held in
tension and the clamp's edge is a re-entrant singularity: the study's peak
von Mises, and the `factor_of_safety` read from it, land on the rim of the
clamp and grow as the mesh is refined. A printed PLA lever on two Ø4.75 mm
pins in Ø5 mm bores reads a factor of 0.1, where its bearing stress is
about 6 MPa, a fifth of yield.

And nothing says so: the headline number is reported the same whether it
comes from the part or from where a support ends.

## The shape of the fix

### `pin`: a rigid pin in a bore (or a shaft in a bearing)

A new support type, `pin` (`LoadType::Pin`). The vocabulary is one word per
kind (`fixed`, `force`, `pressure`, `torque`, `remote`); SolidWorks calls
this a *bearing fixture* or *fixed hinge*, Ansys a *cylindrical support*
(bilateral) or a *compression only support*. `pin` is the thing the user is
holding the part with.

- **Placed on a cylindrical face only.** Its axis is the face's own
  (`fem::CylindricalFaceAxis`, the one a torque takes). Any other face is
  refused, naming what is accepted ("a pin support goes on a cylindrical
  face: a bore or a shaft"), by `add_load`, by the panel's click, and at the
  run for a file that carries one (`CompleteLoadGeometry`).
- **What it holds.** The pin is rigid and does not move: at each node of the
  face it holds the RADIAL displacement (measured from the axis) and the
  AXIAL one, and leaves the TANGENTIAL one free, so the part turns on it
  without friction. `fix_rotation: true` holds the tangential direction too
  (a keyed or pressed pin).
- **Compression only.** A node is held only where the part presses on the
  pin. The radial direction is signed by the face's outward normal, so the
  same rule serves a bore (material outside the pin) and a shaft in a
  bearing (material inside): the support may push into the material, never
  pull it.

**The formulation: a short active-set iteration of linear solves.** Each
iteration is the existing linear solve with a different set of held DOF:

1. Start with every facet of the face in contact.
2. Solve. A facet whose nodes' radial reactions sum to a pull leaves the
   contact set; a facet out of it whose nodes' mean radial displacement
   moves into the pin comes back.
3. Repeat until the set does not change (a cap of 30; not settling is a
   study error, not an answer).

The decision is per facet, not per node, because a tri6 under a uniform
pressure puts no load on its corners and a smoothly varying one can put a
small negative one there: per node, the corners flicker in and out of the
set. A facet is the unit a region is applied in already
(`MeshPlannedStudy`), so the contact's edge is resolved to one row of
facets, and a finer mesh puts it closer.

Why this and not a contact element or a penalty: it is exact frictionless
unilateral contact against a rigid, conforming pin, which is what the
support claims to be; every iteration is a linear solve of the kind already
trusted, held to the same residual; and it needs no penalty stiffness, whose
value would trade constraint violation against the iterative solver's
conditioning. A pin with clearance would press on a narrower arc and raise
the bearing stress; that is not modelled, and the docs say so.

**Held in a rotated frame.** The radial, tangential and axial directions are
not the x, y, z the solver's DOF are in. `SolveCases` takes, beside the fixed
nodes, a list of nodes each with an orthonormal frame and which of its three
local directions are held. Each such node's DOF are expressed in its frame
(`K' = Tᵀ K T` block by block at the scatter, the load vector assembled in
x, y, z and rotated the same way, the answer rotated back), so a held local
direction is eliminated exactly as a fixed DOF is now. K stays symmetric
positive definite and its conditioning is unchanged.

**Reactions.** After each solve the internal forces `Ke uₑ` are summed per
node and the external load subtracted; at a held node that is the support's
reaction. That decides the contact set, and every support's total reaction
(N, x/y/z) is reported in the result, so a lever's two pins can be checked
against its load.

**A pin that leaves a mechanism is refused.** A pin free to turn, alone,
leaves the body free to turn about it, and the loads on a lever do not
balance about one pin. Before each factorization the held constraints are
tested against the six rigid-body motions (the 6x6 Gram matrix of the
constraint rows `[d, x × d]`): a rank below six is a mechanism, refused
before any solve, naming the motion ("free to turn about pin 3's axis: a pin
turns freely, so a lever needs a second support, or set fix_rotation on the
pin"). The same test after the contact set has shrunk catches a load that
lifts the part off its pins ("the loads pull body 'lever' off its pins:
..."). A singular K is never factored, and a garbage answer is never
returned.

**The GPU solve declines a pin, visibly.** The femgpu SpMV applies each
element matrix relative to its first node's translation, which assumes every
node of the element is in x, y, z; a rotated node breaks it. A study with a
pin above the iterative threshold therefore solves on the CPU's PCG, and the
result names it: `solver: "… ; GPU declined: a pin support holds nodes in
rotated frames, which the GPU solve does not take"`.

**Where a pin is refused.** A modal study (a pin's contact depends on the
load; a mode has none), a topology study (it freezes fixed faces), and an
assembly study (its anchors are contacts and imprinted faces; a pin there is
future work), each naming why. A static study of a topology result keeps a
pin: its faces anchor the shape like a fixed one's.

### A peak on a support's edge is flagged

For every static result, per body and per case: the **edge** of a support
is its facets' nodes that also belong to a boundary facet the support does
not hold (a clamp's rim, a pin face's rims). If the peak von Mises node
lies within one element of an edge (a node of a tet that has an edge node),
the result carries `peak_at_support_edge: {support, type,
max_von_mises_away_mpa, at_mm}`: the support, and the largest von Mises
farther than one element from every support's edge. `study_results` reports
it beside `max_von_mises_mpa`, adds `factor_of_safety_away` from it, and the
factor of safety's receipt says its basis sits on a support's edge. The
headline number is unchanged; it is no longer silent.

## Format

`vNEXT`: the load type `pin` and its `fix_rotation` (studies), and in a
static result each case's support `reactions` and `peak_at_support_edge`.
An unknown load type is still refused, naming the accepted ones. A result
saved before vNEXT reads without them.

## Files

- `src/core/fem/FemStudy.{h,cpp}`: `LoadType::Pin`, `Load::fixRotation`,
  the names, `CompleteLoadGeometry` (the axis; refuse a non-cylinder),
  `PlanStudy` (a pin is a support; modal/topology refusals),
  `MeshPlannedStudy` (each pin's facets and frames; every support's facets
  for the edge flag), `SolvePreparedStudy` (the active-set loop).
- `src/core/fem/FemSolver.{h,cpp}`, `FemSolverInternal.h`: held frames in
  `SolveCases`, reactions, the rigid-mode test, the support-edge flag,
  `FemResults::reactions` / `supportEdgePeak`.
- `src/core/fem/FemTopo.h`, `src/femgpu/TopoGpuSolver.cpp`: the decline.
- `src/core/fem/AssemblyStudy.cpp`: the refusal.
- `src/core/io/SerializeStudies.cpp`, `SerializationInternal.h`
  (`kVersionPending`), `tests/golden/vNEXT-pin-support.json` +
  `.expect.json`, `docs/versioning/format.md` (Pending changes).
- `src/remote/RemoteSimulation.cpp`: `add_load {type: pin, fix_rotation?}`
  and its row; `study_results` reporting the reactions and the flag.
- `src/ui/StudyPanel.cpp`: arm Pin beside Fixed; a click on a non-cylinder
  refused in the panel's red line; the pin drawn at its axis.
- `docs/simulation/README.md`: the support, the flag, the tests; the roadmap
  entry goes.

## Verification

`tests/test_fem_pin.cpp` (`[fem][pin]`):

- **A pin-loaded lug against the textbook.** A plate with a hole, pinned,
  pulled along its length: the reaction equals the pull; the mean bearing
  stress `F / (d t)` read on the bore's loaded half; the net-section stress
  `F / ((w − d) t)` across the hole; the contact on the loaded half only
  (about half the bore's area in contact, none on the far side).
- **A lever on two pins.** Two bores, a force at the end: the two pins'
  reactions sum to the load (to the residual) and their moments balance;
  each pin's contact sits on the side the load pushes onto.
- **Fixed against pin on the same lever.** With both bores fixed the peak
  sits on a bore's rim and is flagged; with pins it is several times lower,
  and the factor of safety rises to what the bearing stress says.
- **The flag.** A cantilever clamped at its root: the peak is on the clamp's
  edge and is flagged, with the away value lower; a plate pulled by a force
  far from its clamp is not flagged.
- **Refusals.** A pin on a flat face; one rotation-free pin alone (a
  mechanism, named); a load that lifts the part off its only pins; a pin on
  a modal, topology or assembly study.
- **CPU agreement.** The pinned lug solved direct and iteratively (the
  iterative threshold forced to 0) agree; with a GPU offered, the iterative
  solve says it declined and why.
- **Format.** The golden file round-trips a pin; an unknown load type is
  still refused.

`tests/fem_smoke.py`: `add_load {type: pin}` on a bore, refused on a flat
face, and `study_results` carrying the reactions and the flag. GUI suite
(`sim`): Pin armed and clicked on a bore places a pin; on a flat face it is
refused in the red line and places nothing.
