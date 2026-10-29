# CalculiX as a study solver

## The problem

Linear static, modal and topology studies are solved in-house and stay that
way. Everything past linear is not built: buckling, large deformation,
contact that separates or slides, plasticity and thermal
(docs/simulation/README.md#not-done). That section already names an
external solver as the way to add them, rather than growing ours. A part
that fails by buckling (a slender strut, a thin printed wall, a tube frame
member) currently passes a static study with a comfortable factor of safety.
That is the worst kind of wrong answer, because it looks finished.

CalculiX (`ccx`) does all of these on the element we already mesh. Its
C3D10 tetrahedron has the same node order as `FemMesh::tets` (corners 0–3,
then midsides (0,1), (1,2), (2,0), (0,3), (1,3), (2,3)) and the same
4-point Gauss rule as `kTetGauss` (src/core/fem/FemSolverInternal.h). Its
nodal stress order, xx yy zz xy yz zx, is our Voigt order. Its units are
whatever the deck is written in, so our mm–N–MPa–t system passes through
unchanged.

The bridge itself is built: a static study solves on CalculiX as a second,
independent answer, through the study API, with discovery, the deck, the run
directory, the refusals and the residual check
([docs/simulation/README.md#calculix](../simulation/README.md#calculix)).
What is left is what it was built for: the analyses we do not have. A study
gains a type (`buckling`, then nonlinear options on `static`), solved on
CalculiX through that bridge. Nothing falls back to the in-house solver or to
another analysis.

## What was measured

These numbers are the basis for the plan. They were taken against the
existing build and the nixpkgs ccx, with no code changes.

**Availability**

| where | ccx | how |
|---|---|---|
| nix (our nixpkgs) | 2.23, from the binary cache | `calculix-ccx` (the old `calculix` attribute now throws "renamed"). Deps: spooles, arpack, blas, lapack, libgfortran. 5.6 MB binary, 240 MB closure |
| Ubuntu 24.04 noble | 2.21-1 | `apt install calculix-ccx` (universe) |
| Ubuntu 26.04 resolute | 2.21-1build1 | same |
| Windows | 2.23 | dhondt.de `calculix_2.23_4win.zip`. `ccx_static.exe` (42 MB) bundles SPOOLES and PaStiX statically. The `ccx_dynamic*` variants need MKL's `mkl_rt.2.dll`, which is not shipped. Not cross-buildable from nixpkgs: `pkgsCross.mingwW64.calculix-ccx` is refused as an unsupported system. Running it under Wine was not done: the agent's sandbox denied running the downloaded binary |
| AppImage | none | no host tool is bundled today (`proc::FindOnPath` + `PipedChild` look up ffmpeg on PATH) |
| Flatpak | none | the sandbox hides a host install, so ccx must be built as a module |

**Licence**
- ccx is GPL-2.0-or-later (nixpkgs meta, and ccx's start-up banner).
- SPOOLES is public domain, ARPACK is BSD-3, and libgfortran is GPL-3 with the runtime exception.
- Loböle runs ccx as a separate process that talks to it only through files (an `.inp` in, `.frd`/`.dat` out). That is aggregation, not a combined work, so it has no effect on Loböle's AGPL-3.0-or-later licence.
- Shipping the binary in an AppImage or installer is allowed if:
  - `LICENSES/GPL-2.0.txt` ships with it (it already does, for the fastener tables);
  - we offer the exact corresponding source ourselves. GPL-2 §3(a)/(b) puts that on whoever distributes; a link to upstream counts only for non-commercial redistribution under §3(c). In practice, the ccx source tarball becomes a release asset next to ours.

**Capability** (ccx 2.23 manual)
- `*BUCKLE`: buckling factors are written to `.dat` and mode shapes to `.frd`; `PERTURBATION` adds a preload.
- `*STEP, NLGEOM`: large deformation.
- `*CONTACT PAIR` with `*SURFACE INTERACTION`: penalty or mortar, face-to-face or node-to-face, with friction.
- `*PLASTIC`: isotropic or kinematic hardening.
- Tension-only and compression-only materials.
- `*HEAT TRANSFER`, `*COUPLED TEMPERATURE-DISPLACEMENT`.
- `*EQUATION` and `*TRANSFORM`: arbitrary-direction supports.
- `*COUPLING` + `*DISTRIBUTING`: a remote load as RBE3.
- `*TIE`.

**Accuracy and cost**
- Test piece: a 10×10×200 mm steel column (E 200 GPa, ν 0.3) clamped at the base.
- Meshes: ours is Netgen inside the app; ccx's is gmsh from the exported STEP. They are different meshes of similar size, measured before `export_study_inp` existed.

| case | in-house | ccx 2.23 | closed form |
|---|---|---|---|
| tip deflection, 100 N lateral, 2.5 mm mesh | 1.59761 mm (10 135 nodes) | 1.59788 mm (13 154 nodes) | 1.6000 mm (FL³/3EI) |
| Euler buckling, fixed–free, 2.5 mm mesh | — | 10 286.9 N (+0.06 %) | 10 280.8 N (π²EI/4L²) |
| same, 1.25 mm mesh | — | 10 284.3 N (+0.03 %); mode 3: 91 070 N, 9× theory −1.5 % (shear) | |
| static wall time, 2.5 mm mesh | 5.8 s with mesh | 1.9 s, 150 MB | |
| static, 1.25 mm mesh | 15.2 s with ~4 s mesh (93 k nodes) | 20.7 s, 1.44 GB (79 k nodes, 236 k DOF) | |
| buckling, 1.25 mm mesh, 3 factors | — | 48 s, 1.6–2.2 GB | |

- The nixpkgs build factors on one core ("Using 1 cpu for spooles") whatever `OMP_NUM_THREADS` / `CCX_NPROC_EQUATION_SOLVER` say: SPOOLES is not built multithreaded.
- Memory grows faster than the DOF count, as it does for any direct solver. Roughly 6 KB per DOF for static and 7–10 KB per DOF for buckling at this size.

## The shape of the fix

### Study type and solver choice

- `AnalysisType` gains `Buckling` (src/core/fem/FemStudy.h).
- `auto` becomes CalculiX for buckling and for any nonlinear option (Stage 4
  on); it stays in-house for static, modal and topology. `fem::SolverRefusal`
  gains the refusal of `in_house` on buckling or nonlinear, beside the one of
  `calculix` on modal or topology it has. Modal could map to `*FREQUENCY`
  later; until it does, that stays refused.
- `num_modes` is reused as the number of buckling factors. It defaults to 3, because a symmetric section has two equal first modes, as the square column did.
- `load_cases` is refused on a buckling study: the factor scales one load set.
- A buckling study's input key carries its type and `num_modes` (it does
  through the serialized study, as `solver` does).

### The deck

`fem::WriteDeck` writes the mesh, materials, supports and nodal forces a
static run takes (docs/simulation/README.md#calculix). A buckling study adds:

| ours | `.inp` |
|---|---|
| buckling | `*STEP` / `*BUCKLE` with `num_modes` and accuracy 1e-4 (default 0.01) |
| outputs | `*NODE FILE` U, plus RF for nonlinear runs; `*EL FILE` S, and PE for plasticity |

A pin's contact set is the one the in-house `SolveWithPins` iteration finds
on the same load (`fem::StaticSupportsOf`, which a static run on CalculiX
already uses), held bilaterally. Linear theory scales the load without
changing its direction, so the set holds at every factor. The result says the
set was found that way.

### Running it

The job, the run directory, threads, cancel, `timeout_s`, the memory
refusal and ccx's `*ERROR` lines are built (docs/simulation/README.md
#calculix). Left:
- **Progress.** The job label names the eigenvalue phase; a nonlinear
  step's fraction is its step time, read from `.sta` as each increment
  lands.
- **Memory.** A buckling run's per-DOF cost is calibrated from the Stage 2
  cost test and refuses as a static run's does.
- **Non-convergence.** A nonlinear run that stops converging is refused with
  the load fraction it reached. That fraction is information, not a result,
  so nothing is stored.

### Results: the same shape, checked the same way

- **Buckling's base state** is a linear static run, read and checked as a
  static run on CalculiX is (U from the `.dat`, held to the energy balance
  and the residual against our K).
- **Buckling.** Each body gets:
  - `buckling_factors`: every factor ccx returns, signed. A negative one is the same shape under the reversed load.
  - `buckling_factor`: the smallest positive factor, the governing one. If none of the requested modes is positive, the result says so instead of picking one.
  - mode shapes, stored as modal ones are (`ModeShape` with `factor` beside `frequencyHz`; `KeepWhatIsRead` trims them to boundary nodes).
- **Buckling's honesty rules**, the counterpart of `peak_at_support_edge`:
  - `yield_factor` = yield / peak von Mises of the base (factor 1) state, which is computed anyway. When it is below `buckling_factor`, `yields_first` says the part yields at that multiple before the elastic buckling load is reached. The governing factor is the smaller of the two, and the reply names which.
  - `mode_at_load_edge`: when the first mode's peak motion lies within one element of a support's or a load face's edge, the mode is local crippling where the load enters, not a member buckling. The reply says so and names the face.
  - The receipt's claim is basis `predicted`, with oracle `calculix/buckle` and its version. Its note says linear buckling is an upper bound for imperfection-sensitive shapes (thin shells, tubes).
- **Nonlinear runs (Stage 4 on).** U, S (Cauchy stress at the final increment) and, for plasticity, PE are read from the `.frd`. ccx's equilibrium tolerance is its own (`*CONTROLS`, default 0.5 % of the mean force), not our 1e-6. So the deck tightens it, and the result states the criterion it met and checks that the reactions balance the applied load to that tolerance.
- **The rest is shared.**
  - `set_result_display {mode}` selects a buckling mode the way it selects a modal one.
  - The field renderer, deformed shape and mode animation are unchanged.

### Requirements and sweeps

- `study_result` gains the quantity `buckling_factor`, used with `at_least` (src/core/Requirements.cpp, `MeasureStudyResult` in src/remote/RemoteRequirements.cpp).
- `factor_of_safety` on a buckling study reads `yield_factor`.
- Sweeps already list study requirements as not swept ("a study per point is minutes"), and a buckling one joins that list with the same reason.

### GUI

`StudyPanel.cpp` gets:
- **Study type.** `Buckling` in the type combo; the solver combo (built) offers CalculiX for it.
- **Buckling results.** A "Buckling factors" list in place of "Natural frequencies". Selecting one shows its mode with the deform slider. The governing factor and `yields_first`/`mode_at_load_edge` are shown as text.

### Format

These changes write `vNEXT`:
- the study key `analysis: "buckling"`;
- the result keys `buckling_factors`, `yield_factor` and a mode's `factor`.

Plumbing for the bump:
- raise `kVersionPending`;
- goldens `tests/golden/vNEXT-buckling-study.json` and `.expect.json`;
- a row in format.md's pending table.

Stage 4 onward add the nonlinear keys under their own `vNEXT`.

## Stages

Each stage is useful alone and lands with its docs.

Stage 1 (discovery, the deck, linear static on CalculiX) is built.

**Stage 2: buckling**, ~1 000 lines.
- The analysis type, the pin contact set, the results and their honesty flags, the requirement quantity, the GUI list and mode display, `vNEXT`, and docs/simulation/README.md's new section.
- The "Not done" list drops buckling.

**Stage 3: shipping the binary**, ~300 lines plus packaging.
- **AppImage.** Bundle Ubuntu 24.04's `ccx` and its libraries via linuxdeploy `--executable`.
- **Flatpak.** A ccx 2.23 module.
- **Windows.** The installer carries `ccx_static.exe` as `ccx.exe`, pinned by sha256; `packaging/windows/smoke.sh` runs a buckling deck under Wine.
- **Licensing.** `THIRD_PARTY.md` and `SOURCE-OFFER.md` rows, and the ccx source tarball attached to each release. `release-smoke.py` asserts `ping` finds the bundled solver.

**Stage 4: nonlinear static, large deformation**, ~1 000 lines.
- `static` gains `nonlinear: {large_deformation: true, increments?}`, which maps to `NLGEOM` and `INC`.
- Loads:
  - pressure becomes a follower `*DLOAD P` on element faces. A boundary tri6 maps to its tet's face S1–S4, which needs a face map;
  - a remote load or torque becomes `*COUPLING`/`*DISTRIBUTING` on a reference node;
  - a force stays a dead load, and the docs say so.
- Supporting work: progress from `.sta`, the non-convergence refusal, and the equilibrium check.

**Stage 5: contact**, ~1 500 lines.
- A compression-only support against a rigid occurrence or the ground: the partner is meshed coarse and fixed, with `*CONTACT PAIR`.
- An assembly participant role `contact`: meshed separately rather than fused, its touching faces found by the same fuse `PlanAssemblyStudy` already runs, joined by `*CONTACT PAIR` with `friction` (unitless μ). This lifts the one-meshed-occurrence rule for CalculiX studies only.
- Results: contact pressure and the open/closed area per pair, beside the existing reactions.

**Stage 6: plasticity**, ~800 lines.
- `nonlinear.plasticity: true`, with `*PLASTIC` from the material library.
- Metals gain hardening data. A material without it is refused rather than assumed perfectly plastic (see the questions).
- Results: `max_plastic_strain`, a requirement quantity of the same name, and the residual deformation from an unload step.

**Stage 7: thermal**: steady heat transfer, then thermal stress. Sized when it is next.

## Verification

**Closed forms, through `run_study`, tagged `[calculix]`:**
- **Euler column.**
  - Fixed–free 10×10×200 steel to within 0.5 % of π²EI/4L² (measured: +0.06 % at a 2.5 mm mesh).
  - A 10×20 section: the first two factors in the ratio (b/h)² = 4.
- **Thin walls.** A static study's mesh puts one element through a 0.8–2 mm
  wall at the auto size, and that is within 1 % for bending
  ([fem-mesh-quality.md](fem-mesh-quality.md)). Buckling is checked the same
  way: a 1 mm plate's factor at one layer against two or more layers (Netgen
  `closeedgefac` 2, measured at 5–14× the elements). If one layer is off by
  more than 2 %, a buckling study meshes with `closeedgefac`, and that is the
  only change to its mesh.
- **Wide cantilever plate.** The factor carries the plate stiffness Et³/12(1−ν²), not the beam's EI, to within 2 %. A classical simply supported plate needs a one-direction (roller) support that the study does not have.
- **Stage 4.** The elastica: a cantilever's end deflection at PL²/EI = 1 against the Bisshopp–Drucker solution.
- **Stage 5.** Hertz contact width for a sphere on a block; a block lifting off a rigid floor under a moment.
- **Stage 6.** A bar past yield (uniform, exact), and a rectangular beam's collapse load M_p = σ_y b h²/4.

**Refusals, in `refused_calls_smoke` and `tests/test_fem_calculix.cpp`:**
`in_house` on buckling; load cases on buckling; non-convergence (Stage 4).

**The MCP-only path:** `tests/calculix_smoke.py` builds a column, adds a buckling study and a `buckling_factor` requirement, and reads the factor and `check_requirements`, using RPCs only. It is gated on `ping`'s `features.calculix`.

**Cost:**
- A `[slow]` CPU-time check of a ~240 k-DOF buckling run, against the measured ~48 s.
- A buckling run's memory coefficients are taken from the same run, so its estimate cannot drift from what the solver costs.

**Docs:**
- docs/simulation/README.md's "CalculiX" section gains buckling (its honesty rules) and loses buckling (later, the rest) from "Not done".
- docs/packaging/README.md gains the bundled solver.
- The RPC `describe` rows for `add_study`, `set_study`, `study_results` and `set_result_display`.

## Files

- New: `tests/calculix_smoke.py`; the goldens.
- `src/core/fem/Calculix.{h,cpp}`: `*BUCKLE`, the factors read from the `.dat`, the mode shapes from the `.frd`; `tests/test_fem_calculix.cpp`.
- `src/core/fem/FemStudy.{h,cpp}`: the type, its refusals, the solve branch.
- `src/core/fem/FemSolver.h`: `ModeShape::factor`, buckling fields.
- `src/core/io/SerializeStudies.cpp`, `src/core/io/SerializationInternal.h`, `docs/versioning/format.md`.
- `src/remote/RemoteSimulation.cpp`: rows and handlers.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp`.
- `src/ui/StudyPanel.cpp`.
- The python client's `rpc.py` and `rpc-surface.json`, regenerated.
- `packaging/appimage/*`, `packaging/flatpak/org.lobole.Lobole.yaml`, `packaging/windows/{installer.nix,installer.nsi,smoke.sh}`.
- `THIRD_PARTY.md`, `LICENSES/SOURCE-OFFER.md`.
