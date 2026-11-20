# FEM reference cases

A suite of published benchmark problems the solvers must reproduce, each
with its source and a reasoned tolerance, and the supports those problems
need.

## The problem

- **The solver is checked against closed forms we chose.** `tests/test_fem*.cpp`
  hold a bar, a cantilever, Lamé, a modal beam, Euler, SP-8007 and Hertz.
  There is no NAFEMS or MacNeal–Harder case, so nothing a reader can look
  up says "this element and this procedure reproduce the published target",
  and tet10 has never had a near-incompressible (locking) check or a
  convergence-rate check: a single-mesh tolerance cannot see an element
  bug that a rate does.
- **The "Measured" tables in `docs/simulation/README.md` are copied by
  hand**, so they go stale the way a written-down count does.
- **The standard cases cannot be set up at all.** A support is `Fixed` (all
  three directions), `Pin` or `Contact` (`fem::LoadType`,
  `src/core/fem/FemStudy.h`). There is no symmetry plane, no frictionless
  (roller) support, no single held direction, no prescribed displacement
  and no cylindrical support with radial, axial and tangential each held or
  free. NAFEMS LE1, LE10, LE11 and FV32 and every quarter model need them;
  so does a user who wants to halve a symmetric part or let it slide on a
  flat.

## The shape of the fix

### 1. The supports

New support kinds on the frames `MakeReduction` (`FemSolver.cpp`) already
holds DOF in (`HeldNode`), which `FreeRigidMotion` already reads:

- **Roller** (`roller`): zero displacement along the face normal. On a flat
  face it is a symmetry plane; on a curved one a frictionless seat. An
  inclined plane works as an axis-aligned one does.
- **Held directions** (`held`): any of x, y, z of the world or of a named
  datum frame held, each at zero or at a prescribed displacement
  (`dx_mm`, `dy_mm`, `dz_mm`).
- **Cylindrical** (`cylindrical`): on a cylinder, radial, axial and
  tangential each held or free. Unlike a pin it holds both ways and has no
  contact iteration.

Each refuses what it cannot mean (a roller's normal on a face with none, a
cylindrical support off a cylinder, a prescribed displacement on a modal or
buckling study), naming what is accepted. A node two supports hold in
frames that disagree is refused naming both, as pins that share a node are.
Reactions are reported per support as now. On CalculiX they are written as
`*TRANSFORM` + `*BOUNDARY`. Serialized (`vNEXT`, golden), RPC rows with
unit suffixes, the Simulation window's support tools with their viewport
glyphs (a roller's hatches on wheels, a held direction's arrows), and
`check_literals`-style defaults: nothing here carries a default with a
unit.

### 2. The cases

`tests/fem_reference/<case>/`: `case.py` builds the part and the study
through the RPC (so the case tests the pipeline a user drives, mesher
included), and `expected.json` holds, per quantity: `kind` (theory |
benchmark), `source` (document, table, page), `value`, `unit`, `tolerance`
and a written `reason` for that tolerance (the mesh size it was judged at,
what the element can and cannot resolve). The rules:

- a reference is a published value, never a number from running another
  solver; ccx is a cross-check, reported beside it, not the reference;
- a tolerance is never widened to pass. A miss with a known cause is a
  `known_failure` with the cause, which stays in the generated page, and a
  known failure that starts passing fails the run until it is removed.

The first cases: NAFEMS LE1 (elliptic membrane, as a thin solid), LE10
(thick plate, σyy at D), LE11 (solid cylinder/taper/sphere under a
temperature field), FV32 (tapered membrane frequencies); MacNeal–Harder's
thick cylinder at ν = 0.49, 0.499, 0.4999; Kirsch's plate with a hole
(Kt = 3); a cantilever convergence-rate case (error against mesh size,
monotone, rate above a floor stated with its reason); and the existing
closed forms re-expressed as cases, so the Catch2 ones stay as fast unit
checks and the suite is the citable record. Buckling, thermal, contact and
plasticity get a case each from what their sections already measure.

### 3. The record

`tools/fem_reference.py run` runs every case (in-house, and on ccx where
found) and writes a generated page, `reference-cases.md`, beside the simulation
README: per case the
source, the target, what each solver got, the error, the tolerance and
its reason, and the known failures. The page is generated and committed;
a `[docs]`-style check fails when the committed page differs from a run's.
`docs/simulation/README.md`'s "Measured" tables that a case now covers are
replaced by links to it. `describe` rows and a study's receipt name the
cases that verify the procedure a result came from ("static tet10: LE1,
LE10, Lamé, Kirsch"), so an agent can say what the answer rests on.

## Files

- `src/core/fem/FemStudy.{h,cpp}`, `FemSolver.cpp`, `Calculix.cpp` (the
  deck), `src/core/io/SerializeStudies.cpp` (`vNEXT`, golden).
- `src/remote/RemoteSimulation.cpp` and its `describe` rows;
  `src/ui/StudyPanel.cpp` (tools, glyphs).
- `tests/fem_reference/`, `tools/fem_reference.py`, a ctest entry (label
  `fem_reference`) under a ctest configuration of its own, outside the
  headless suite, which is long enough: its own row in CLAUDE.md's
  table).
- `tests/test_fem_supports.cpp`.
- `docs/simulation/README.md` and the generated page beside it,
  `docs/versioning/format.md`.

## Verified by

- Supports: a half bar on a roller equals the whole bar; a Lamé quarter
  cylinder on two rollers matches the closed form; an inclined symmetry
  plane matches its axis-aligned image; a prescribed displacement gives the
  reaction the stiffness predicts; each refusal; ccx agreeing on the same
  mesh; save and reopen.
- The suite: every case inside its tolerance or a named known failure; a
  planted element bug (a wrong Gauss weight behind a test switch) fails the
  convergence-rate case and the locking case.
- A GUI test placing a roller and a held direction by clicking.
- The headless, GUI and pygui suites.
