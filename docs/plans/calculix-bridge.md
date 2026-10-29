# CalculiX as a study solver

## The problem

Linear static, modal, buckling and topology studies are solved in-house and
stay that way. Plasticity and thermal are not built
(docs/simulation/README.md#not-done). That section names an external
solver as the way to add them, rather than growing ours, as large
deformation, snap-through and contact were added. A part loaded past
yield carries its load by spreading the yield, which a linear study
cannot see.

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
Linear buckling is solved in-house, with CalculiX's `*BUCKLE` as its
cross-check ([docs/simulation/README.md#buckling](../simulation/README.md#buckling)),
large deformation and snap-through are `static`'s `nonlinear` option on
CalculiX
([docs/simulation/README.md#nonlinear-static](../simulation/README.md#nonlinear-static)),
and contact is a support and a participant role solved on the same path
([docs/simulation/README.md#contact](../simulation/README.md#contact)).
What is left is plasticity and thermal, each a `nonlinear` key on the same
path. Nothing falls back to the in-house solver or to another analysis.

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

Each remaining analysis is a key of `static`'s `nonlinear` object, solved by
`fem::NonlinearOnCalculix` as large deformation is: the deck from the
study's own mesh, the run watched through the `.dat` and `.sta`, the
non-convergence refusal with the load reached, and the equilibrium check.
`in_house` with any of them is refused, as `nonlinear` is now. Each stage
adds its keys under its own `v208`.

## Stages

Each stage is useful alone and lands with its docs. Stages 1 (discovery, the
deck, linear static on CalculiX), 3 (shipping ccx in the packages,
[docs/packaging/README.md#the-bundled-calculix](../packaging/README.md#the-bundled-calculix)),
4 (large deformation, snap-through) and 5 (contact) are built, and linear
buckling with them (in-house, `*BUCKLE` its cross-check).


**Stage 6: plasticity**, ~800 lines.
- `nonlinear.plasticity: true`, with `*PLASTIC` from the material library.
  The path past a peak is not followed (ccx 2.23 has no arc-length method:
  `*STATIC, RIKS` is read as a plain `*STATIC` with a warning), so a
  collapse load is the factor the load-controlled run stops at, as a
  snap-through run's limit point is.
- Metals gain hardening data. A material without it is refused rather than assumed perfectly plastic.
- Results: `max_plastic_strain`, a requirement quantity of the same name, and the residual deformation from an unload step.

**Stage 7: thermal**: steady heat transfer, then thermal stress. Sized when it is next.

## Verification

**Closed forms, through `run_study`, tagged `[calculix]`:**
- **Stage 6.** A bar past yield (uniform, exact), and a rectangular beam's collapse load M_p = σ_y b h²/4.

**Refusals, in `refused_calls_smoke` and `tests/test_fem_calculix.cpp`:**
each new key refused where `nonlinear` already is (in-house, another
analysis, load cases), and a material without hardening data (Stage 6).
Contact's are in place (docs/simulation/README.md#contact): the plasticity
and thermal keys join them.

**Docs:**
- docs/simulation/README.md's "CalculiX" section gains each stage, and "Not done" loses it.
- The RPC `describe` rows each stage touches.

## Files

- `src/core/fem/Calculix.{h,cpp}`: the plastic and thermal deck keys, reading PE; `tests/test_fem_calculix.cpp`.
- `src/core/fem/FemStudy.{h,cpp}`: the `nonlinear` keys and their refusals.
- `src/core/io/SerializeStudies.cpp`, `src/core/io/SerializationInternal.h`, `docs/versioning/format.md`, a golden per stage.
- `src/remote/RemoteSimulation.cpp`: rows and handlers.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp` (Stage 6's quantity).
- `src/ui/StudyPanel.cpp`.
- The python client's `rpc.py` and `rpc-surface.json`, regenerated.
