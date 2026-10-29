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
What is left is what it was built for: the nonlinear analyses we do not
have, as options on `static`, solved on CalculiX through that bridge. Linear
buckling is solved in-house instead (docs/plans/buckling.md). Nothing falls back to the in-house solver or to
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

Linear buckling is solved in-house (docs/plans/buckling.md), with CalculiX as
its test oracle and as an optional second answer. What is left here is the
nonlinear analyses, which stay CalculiX's.

### Study options and solver choice

- `static` gains `nonlinear: {...}` options (Stage 4 on). `auto` becomes
  CalculiX for any study that sets one, and `fem::SolverRefusal` gains the
  refusal of `in_house` on a nonlinear option.
- A nonlinear option's input key carries it, through the serialized study, as
  `solver` does.

### Running it

The job, the run directory, threads, cancel, `timeout_s`, the memory refusal
and ccx's `*ERROR` lines are built (docs/simulation/README.md#calculix). Left:
- **Progress.** A nonlinear step's fraction is its step time, read from `.sta`
  as each increment lands.
- **Memory.** A nonlinear run's per-DOF cost is calibrated from the Stage 4
  cost test, and it refuses as a static run's does.
- **Non-convergence.** A nonlinear run that stops converging is refused with
  the load fraction it reached. That fraction is information, not a result, so
  nothing is stored.

### Results

- U, S (Cauchy stress at the final increment) and, for plasticity, PE are read
  from the `.frd`. ccx's equilibrium tolerance is its own (`*CONTROLS`, default
  0.5 % of the mean force), not ours. So the deck tightens it, and the result
  states the criterion it met and checks that the reactions balance the
  applied load to that tolerance.
- The field renderer, deformed shape and result display are unchanged.

### Format

Each stage adds its keys under its own `vNEXT`.

## Stages

Each stage is useful alone and lands with its docs. Stage 1 (discovery, the
deck, linear static on CalculiX) is built. Buckling is
docs/plans/buckling.md.

**Stage 4: nonlinear static, large deformation**, ~1 000 lines.
- `static` gains `nonlinear: {large_deformation: true, increments?}`, which maps to `NLGEOM` and `INC`.
- Loads:
  - pressure becomes a follower `*DLOAD P` on element faces. A boundary tri6 maps to its tet's face S1–S4, which needs a face map;
  - a remote load or torque becomes `*COUPLING`/`*DISTRIBUTING` on a reference node;
  - a force stays a dead load, and the docs say so.
- **Snap-through and post-buckling.** An option seeds the geometry with the
  first buckling mode (docs/plans/buckling.md) scaled to a stated
  imperfection, and steps with `*STATIC, RIKS`. The result reports the peak
  load factor it reached: the answer for an arch or a curved wall loaded
  across its curve, which linear buckling cannot see.
- Supporting work: progress from `.sta`, the non-convergence refusal, and the equilibrium check.

**Stage 3: shipping the binary**, ~300 lines plus packaging. It lands with
Stage 4: until a nonlinear study exists, nothing a user needs requires a
bundled ccx.
- **AppImage.** Bundle Ubuntu 24.04's `ccx` and its libraries via linuxdeploy `--executable`.
- **Flatpak.** A ccx 2.23 module.
- **Windows.** The installer carries `ccx_static.exe` as `ccx.exe`, pinned by
  sha256; `packaging/windows/smoke.sh` runs a deck under Wine.
  `PipedChild::Poll` blocks on Windows today, so `timeout_s` and cancel cannot
  interrupt a run there, and dead run directories are not swept: both are
  fixed here.
- **Licensing.** `THIRD_PARTY.md` and `SOURCE-OFFER.md` rows, and the ccx source tarball attached to each release. `release-smoke.py` asserts `ping` finds the bundled solver.

**Stage 5: contact**, ~1 500 lines.
- A compression-only support against a rigid occurrence or the ground: the partner is meshed coarse and fixed, with `*CONTACT PAIR`.
- An assembly participant role `contact`: meshed separately rather than fused, its touching faces found by the same fuse `PlanAssemblyStudy` already runs, joined by `*CONTACT PAIR` with `friction` (unitless μ). This lifts the one-meshed-occurrence rule for CalculiX studies only.
- Results: contact pressure and the open/closed area per pair, beside the existing reactions.

**Stage 6: plasticity**, ~800 lines.
- `nonlinear.plasticity: true`, with `*PLASTIC` from the material library.
- Metals gain hardening data. A material without it is refused rather than assumed perfectly plastic.
- Results: `max_plastic_strain`, a requirement quantity of the same name, and the residual deformation from an unload step.

**Stage 7: thermal**: steady heat transfer, then thermal stress. Sized when it is next.

## Verification

**Closed forms, through `run_study`, tagged `[calculix]`:**
- **Stage 4.**
  - The elastica: a cantilever's end deflection at PL²/EI = 1 against the
    Bisshopp–Drucker solution.
  - A shallow arch's snap-through load against its closed form.
  - A column seeded with its first mode reaches a peak load factor below its
    linear one and approaching it as the imperfection shrinks.
- **Stage 5.** Hertz contact width for a sphere on a block; a block lifting off a rigid floor under a moment.
- **Stage 6.** A bar past yield (uniform, exact), and a rectangular beam's collapse load M_p = σ_y b h²/4.

**Refusals, in `refused_calls_smoke` and `tests/test_fem_calculix.cpp`:**
`in_house` on a nonlinear option; non-convergence.

**Docs:**
- docs/simulation/README.md's "CalculiX" section gains each stage, and "Not done" loses it.
- docs/packaging/README.md gains the bundled solver (Stage 3).
- The RPC `describe` rows each stage touches.

## Files

- `src/core/fem/Calculix.{h,cpp}`: the nonlinear deck keys, `.sta` progress, reading S and PE; `tests/test_fem_calculix.cpp`.
- `src/core/fem/FemStudy.{h,cpp}`: the options, their refusals, the solve branch.
- `src/core/io/SerializeStudies.cpp`, `src/core/io/SerializationInternal.h`, `docs/versioning/format.md`, a golden per stage.
- `src/remote/RemoteSimulation.cpp`: rows and handlers.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp` (Stage 6's quantity).
- `src/ui/StudyPanel.cpp`.
- The python client's `rpc.py` and `rpc-surface.json`, regenerated.
- `packaging/appimage/*`, `packaging/flatpak/org.lobole.Lobole.yaml`, `packaging/windows/{installer.nix,installer.nsi,smoke.sh}`, `src/common/ChildProcess.cpp` (Windows polling).
- `THIRD_PARTY.md`, `LICENSES/SOURCE-OFFER.md`.
