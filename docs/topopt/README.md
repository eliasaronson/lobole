# Topology optimization

Generative design: given a design envelope, the faces it is fixed by and the
loads it carries, the optimizer removes material down to a target volume
fraction while keeping the part as stiff as possible. The result is an
organic shape to print from, or to re-model clean parametric geometry
against.

It is a third kind of study (`AnalysisType::Topology` on `fem::Study`), so it
shares the meshing, materials, face references, loads, staleness and panel of
the FEM studies described in [../simulation/README.md](../simulation/README.md).

## The method

SIMP with an optimality-criteria update (`fem::TopologyOptimize`,
`src/core/fem/FemTopo.{h,cpp}`), on the same tet10 mesh and element math as
the static solver (`src/core/fem/FemSolverInternal.h`, shared verbatim):

1. Each tet's density ρ is a design variable in [0, 1]; its modulus is
   `Emin + ρᵖ (E0 − Emin)` with p = 3 and `Emin = 1e-6 E0` (modified SIMP).
   The penalty pushes intermediate densities toward 0 or 1.
2. Solve `K(ρ) u = F` for the study's force and pressure loads.
3. Sensitivity of compliance per element, `−p ρᵖ⁻¹ uₑᵀ Kₑ⁰ uₑ` — the element
   strain energy at full modulus.
4. **Sensitivity filter** (Sigmund) over element centroids with radius R,
   found through a uniform grid hash. The physical density equals the design
   variable, so the field goes crisply to 0/1 and the volume stays linear in
   the variables. R is `filterRadius` (mm), or 2.5 × the mean element size
   when 0; it sets the minimum member size.
5. OC update: bisect the Lagrange multiplier on the volume constraint, move
   limit 0.2, damping 0.5.
6. Stop when the largest density change is below 0.01, or at
   `maxIterations` (default 60).

Invariants:

- **Every tet touching a fixed node, a loaded boundary triangle or a keep
  face is frozen at density 1**, so supports, load regions, bolt bosses and
  bearing seats never vanish. The rest starts uniform at the value that meets
  the target volume.
- **Gravity is excluded.** Self-weight is a design-dependent load (F depends
  on ρ); the objective uses only the study's face loads.
- **Penalty is constant.** `TopoParams::penaltyRampIters` exists but is 0:
  sensitivity filtering already yields a discrete design at constant p, and
  ramping p from 1 converges to a softer, greyer design.
- **One body.** The design domain is the single loaded body; modelled holes
  are keep-out by construction. A study loading several bodies is refused.
- The CPU linear solver is Eigen CG with an incomplete-Cholesky
  preconditioner, warm-started from the previous iteration's displacement (K
  changes little between SIMP steps). This is the correctness oracle and the
  fallback for the GPU solver below.

Validation happens before the run, on `study.error`: a fixed support and a
force or pressure load, volume fraction in (0, 1), a material with stiffness
data, a single loaded body.

## Results

A topology result **is saved with the project**, unlike FEM results: it
costs minutes, and it is tied to a mesh that Netgen does not reproduce across
versions or machines, so re-running would not give it back. It is written as
`studies/<id>-<slug>.result.json` beside the study (flat arrays: mesh nodes
and tets, per-tet density, compliance history, iterations, achieved volume
fraction, and `input_hash`, the hash of what produced it) and loaded into
the same `StudyRuntime` slot FEM results use. On load it is stamped with
the settled document version, so it reads current until the part changes --
unless its saved hash no longer matches the reopened part, when it reads
stale; a stale result is still displayed, badged. A run of the same inputs
after a reopen is answered from it, `reused: true`, nothing computed
([studies as jobs](../simulation/README.md#studies-as-jobs)). The solver
name and wall time are not saved. Goldens use a hand-made mesh, never Netgen
output.

Display and export (`FemTopo.cpp`, pure geometry, so a saved result displays
on a build without the solver or mesher):

- `fem::ExtractIsoSurface`: densities averaged to corner nodes, marching
  tetrahedra over each tet's corners (midside nodes ignored), capped at the
  domain boundary, facet normals outward.
- `fem::AutoIsoThreshold`: the threshold whose solid volume matches the
  achieved fraction, by bisection; used when the threshold is 0.
- The **Shape** view is that surface as a grey `MeshRenderer` body; the
  **Density** view colours the node-averaged density through `FieldRenderer`.
  The design body is hidden while either is shown.
- The **shape guide** (`set_topo_ghost`, or the panel's "Shape guide
  (modeling overlay)") draws the extracted surface translucent in normal
  modelling, to sketch and extrude against. There is no snapping to it.
- `fem::ExportTopoStl`: extract, weld, Taubin (λ/μ) smoothing, which fairs
  the staircase without shrinking, then a binary STL. It reports triangle and
  boundary-edge counts and whether the surface is watertight.

There is no automatic reconstruction of a B-Rep or feature tree from the
result. The workflow is to re-model over the ghost, then check the new part
with a static study.

## The run

A run takes minutes, so it happens on a worker thread over an immutable
snapshot. `fem::PrepareTopology` (main thread) evaluates expressions,
resolves faces, meshes the body and builds a `fem::TopoInput` of plain data;
the worker runs `TopologyOptimize` with no document access, publishing
iteration, compliance, largest change and volume fraction through atomics,
and checking a cancel flag each iteration. The result is stored at the one
join point on the main thread (`ui::StartTopologyRun`,
`src/ui/SimulationActions.cpp`). Cancelling keeps the best densities so far
as a valid result marked cancelled; a cancelled result is never reused for
a later run. The run is a job with an id, cancelled with the RPC connection
that started it unless `outlive_client`
([studies as jobs](../simulation/README.md#studies-as-jobs)). One study run
at a time.

## The GPU solver

`src/femgpu/` builds the `fem_gpu` static library (Vulkan compute, no
surface). `designer_core` stays GPU-free: `TopologyOptimize` takes an
optional `fem::TopoLinearSolver` (the seam in `FemTopo.h`), and the app
passes a `gpu::TopoGpuSolver` when a usable hardware device exists. The build
option is `DESIGNER_ENABLE_FEM_GPU` (default ON); compute shaders are
compiled to embedded SPIR-V (`src/shaders/fem_*.comp`, `cg_*.comp`,
`cheb_*.comp`, `mg_*.comp`).

- **Matrix-free SpMV** (`gpu::TopoSpmv`, `fem_spmv.comp`): one workgroup per
  tet applies `scale_e · Kₑ⁰` to its 30 DOF and scatters with float
  `atomicAdd` (requires `VK_EXT_shader_atomic_float`). K is never assembled.
  The product is taken relative to the element's first node (see
  [below](#mixed-precision-refinement)): identical to `Kₑ⁰·p` in exact
  arithmetic, and it keeps a rigid translation exactly unloaded in fp32.
- **On-device PCG** (`gpu::TopoCgSolver`): the whole loop runs in compute
  shaders recorded in 32-iteration chunks, reading back one scalar per chunk.
  fp32 needs three guards: the true residual replaces the recurrence one at
  chunk boundaries (the recurrence reads optimistic); the best iterate is
  what is returned (fp32 CG drifts past its round-off floor); a stall
  restarts from the best iterate, and two restarts with no gain end the
  solve. Inside the refinement loop below it runs in *last-iterate* mode
  instead: no best-iterate bookkeeping, no restarts.
- **Multigrid preconditioner** (default; `gpu::TopoMultigrid`,
  `mg_restrict.comp`, `mg_prolong.comp`): a two-level V(1,1) cycle. Nodes are
  aggregated once per run; each aggregate gets 6 coarse DOF (rigid-body
  modes about its centroid — piecewise-constant prolongation cannot represent
  rotation). The Galerkin coarse operator is reassembled each SIMP iteration
  from per-element blocks precomputed at setup and factorized on the CPU in
  fp64; Chebyshev on the Jacobi-scaled operator is the smoother, aimed by a
  power-iteration estimate of λmax(D⁻¹K) started from a fixed pseudo-random
  vector. (Started from ones — the smoothest vector, nearly orthogonal to the
  top eigenvector — it read 3.35 on a real SIMP system whose λmax was 5.52;
  the smoother then amplified the top of the spectrum and the PCG made no
  progress in 3000 iterations.) The coarse
  space is what removes the low-energy void-region modes that polynomial
  preconditioning cannot reach. If the hierarchy cannot be built, Jacobi is
  used.
- **Element energies in fp64** (`TopoLinearSolver::ElementEnergies`): the
  sensitivities' `uₑᵀ Kₑ⁰ uₑ`, computed on the host from fp64 element
  matrices `TopoGpuSolver` keeps (the upper triangle, about the size of the
  fp32 copy). `TopoCgSolver::ElementEnergies` (`fem_energy.comp`) still
  computes them on the device from fp32 Ke, but on a slender part that is
  the small difference of large terms (below) and sends SIMP to a different
  design, so the adapter does not use it. A solver may decline, and the CPU
  loop (the oracle) runs instead.

Device choice: discrete over integrated; the settings' `gpuDevice` names one.
Software devices (llvmpipe) lose to the CPU and are not used by the app.
Environment overrides: `DESIGNER_TOPO_GPU=0` forces the CPU, `=1` admits a
software device; `FEMGPU_PRECOND=mg|jacobi|cheb`, `FEMGPU_CHEB_DEGREE`,
`FEMGPU_CHEB_RATIO`. Diagnostics: `TOPO_PROFILE=1` prints per-phase SIMP
timing, `FEMGPU_SOLVE_TRACE=1` one line per linear solve, `FEMGPU_CG_TRACE=1`
single-iteration chunks.

### Mixed-precision refinement

fp32 alone cannot solve a slender bending part. Dense fp64 eigenvalues of the
reduced K (`femgpu_tests [fp32floor]`, "condition numbers"):

| case | diagonal spread | κ(K) | κ after diagonal scaling |
|---|---|---|---|
| cube 1:1, uniform | 12 | 3.3e+03 | 2.2e+03 |
| beam 10:1, uniform | 12 | 3.7e+06 | 2.4e+06 |
| beam 10:1, frozen skin + 0.027 | 220 | 8.2e+07 | 1.6e+07 |
| beam 20:1, uniform | 12 | 1.4e+07 | 9.4e+06 |
| beam 20:1, frozen skin + 0.027 | 170 | 2.7e+08 | 5.3e+07 |

Slenderness costs ~4× in κ per doubling, SIMP's frozen skin (density 1
against a bulk at the volume fraction cubed) another ~20×, and refining the
mesh makes it worse (κ ∝ h⁻²). Worse than the conditioning, rounding Ke to
fp32 changes the *operator*: Kₑ annihilates a rigid translation, a slender
part's elements mostly translate, and the rounded Ke's ~1e-7 error times a
translation thousands of times the element's strain made a beam 20–60 %
too stiff ("fp32 Ke as an operator on slender parts"). `fem_spmv.comp`
therefore works relative to each element's first node — `Sᵀ Ke' S`, Ke
without node 0's rows and columns, S subtracting node 0's displacement —
which is Ke in exact arithmetic and brings the error to ~1e-3; the multigrid
coarse blocks use the same form.

`TopoGpuSolver::Solve` wraps the fp32 GPU CG in fp64 refinement:

```
r = F - K·u                              (fp64, host)
repeat until |r|/|F| < 1e-5:
    d = fp32_cg(K, r/|r|) · |r|          (GPU, loose: 1e-1, ≤128 iterations)
    p = d - (d·Kp_prev / p_prev·Kp_prev) p_prev   (fp64)
    u += α p,  r -= α K·p,  α = p·r / p·Kp        (fp64)
```

- **The fp64 side** applies K from the fp64 element matrices on the host,
  gathered by row (no atomics, fixed row blocks across threads, bit-identical
  run to run): one product per step. On the 32,805-tet pan/tilt yoke a
  product is ~15 ms against a 1.2–4 s solve, so an fp64 GPU kernel (at 1/64
  rate on the RTX 4060) could save at most ~4 %; there is none.
- **The steps are a flexible CG** (FCG(1)): each correction is
  K-orthogonalized against the previous one and taken with the
  energy-optimal length. An exact inner solve makes that plain refinement;
  an inexact one cannot raise the energy error, since a step along any
  direction minimizes it.
- **The right-hand side is rescaled** to unit norm, so the shrinking residual
  never wastes fp32's exponent range.
- **The inner solve returns its last iterate** (`TopoCgSolver::Solve`,
  `lastIterate`). CG lowers the energy error every step while the residual
  norm first rises — by orders of magnitude on a bending load — so the best
  iterate by norm was the zero it started from, and refinement would step by
  nothing forever.
- **Progress is measured by the potential energy** Π(u) = ½uᵀKu − Fᵀu, which
  every step lowers by exactly ½α·p·r; the residual norm is not monotone.
  The solve stops at the 1e-5 target, after three steps that gain less than
  1e-13 of Π, or after 40 steps.
- **The warm start** (the previous SIMP iteration's u) is first scaled by its
  energy-optimal factor for the new stiffness — never worse than it or zero.
- The inner tolerance 1e-1 measured fastest: 1e-2 spent iterations solving
  the fp32 operator more exactly than it is worth.

A solve that ends no better than u = 0 (relative residual ≥ 0.99) still
fails, naming it, and `TopologyOptimize` then drops the external solver and
finishes the run on the CPU (`TopoResult::solverFallback`; `study_status`
says so from that iteration). A zero displacement field would read
downstream as a converged design. A dropped solver is not retried.

Measured on an RTX 4060 Laptop against the CPU path (Eigen CG-IC) and an
exact fp64 factorization (`femgpu_tests [refinebench]`, a few SIMP
iterations each; the GPU and CPU runs land on the same design, 0 flips):

| part | tets | CPU s/iter | GPU before | GPU s/iter |
|---|---|---|---|---|
| beam 20:1, 200×10×20 | 1,920 | 0.88 | 1.06 (fell back) | 0.24 |
| beam 20:1, 200×10×20 | 15,360 | 21.3 | 24.7 (fell back) | 1.40 |
| beam 40:1, 300×7.5×15 | 12,960 | 30.7 | fell back | 1.13 |
| cube 80³ | 10,368 | 3.7 | 1.35 | 0.69 |
| pan/tilt yoke, 8.1 mm (app, 30 iterations) | 32,805 | 244 | fell back | 2.7 |

Below a few hundred tets the CPU is as fast. The yoke's 244 s is the run that
fell back (7313 s); its GPU run is 80 s, the same compliance history.

The GPU solver is linked into `designer_app`, the library the run is
started from (`ui::StartTopologyRun`); for a month it was linked into the
`designer` executable only, where the define never reached that file, and
every run went to the CPU. `ui::TopologyGpuBuiltIn` and a test in
`tests/test_actions.cpp` hold the library to the build option.

## Using it

### GUI

1. Model a **design envelope**: a solid that bounds where material may go.
   A bar or bracket gives the optimizer room to form struts; a cube fixed and
   loaded on opposite faces has no room for a load path. Assign a material
   with stiffness data.
2. Enter simulation, add a study and set the analysis to **Topology**.
3. Arm **Fixed** and click the support faces; arm **Force** or **Pressure**
   and click the loaded faces. Optionally arm **Keep face** for faces that
   must stay solid, and set a mesh size.
4. Set the **Volume fraction** (0.3 keeps 30 % of the material), and
   optionally the filter radius and max iterations. **Run** shows a progress
   bar with the iteration; **Cancel** keeps the best shape so far.
5. The result shows iterations, a compliance start→end, the achieved volume
   fraction and a stale badge. **Display**: *Shape* (the grey optimized body,
   with a **Threshold** slider; 0 = volume-matched), *Density* or *Off*.
6. Tick **Shape guide (modeling overlay)** and leave simulation to model
   against the result.

Volume fractions of 0.4–0.5 give chunkier, cleaner members than 0.3 on small
parts; lower fractions need finer meshes. A finer mesh gives a crisper shape
at a higher cost per iteration.

### RPC

```
add_study {type: "topology", name?}                -> {id}
set_study {id, mesh_size?, volume_fraction?, filter_radius?, max_iterations?}
add_load  {study, type: "fixed", face}
add_load  {study, type: "force", face, magnitude, direction?}   # N, total on the face
add_keep_face {study, face}      / remove_keep_face
run_study {id, recompute?, outlive_client?}
                    -> {started: true, job, kind}      (always async)
                    or {reused: true, started: false, input_hash, ...}
study_status {id}   -> {running, job, iteration, max_iterations, compliance,
                        max_change, volume_fraction, solver, elapsed_seconds}
study_status {job}  -> {running, fraction, ...} or {outcome, ...}
cancel_study {id | job}                            -> {cancelled, job}
study_results {id}  -> {iterations, converged, cancelled, volume_fraction,
                        compliance, solver, solve_seconds, stale}
set_result_display {study, field: "shape"|"density"|"none", threshold?}
set_topo_ghost {study, on}
export_topo_stl {study, path, threshold?, smooth?}  # smooth: Taubin passes, default 3
```

`volume_fraction` and `filter_radius` also take expression strings. Export
to STL is RPC-only; the panel has no button for it.

## Tests

- `tests/test_topopt.cpp` (`[topopt]`): the SIMP gate on a deterministic,
  Netgen-free structured tet10 cantilever (volume target, compliance drop,
  frozen skin solid, a recognizable truss), the same problem through Netgen,
  cancel keeping best-so-far, input refusals, the injected-solver seam,
  `RunTopology` end to end and its single-body refusal, result persistence
  and JSON round-trips, iso-surface closure and orientation,
  `AutoIsoThreshold`, STL output. `[.][topobench]` is a manual benchmark.
- `tests/test_femgpu.cpp` (`femgpu_tests`, every case SKIPs without a
  compute device): SpMV and element energies against the CPU on a structured
  and a Netgen mesh; CG to tolerance; `TopologyOptimize` via the GPU landing
  on the CPU design (the same run: compliance history to 1e-4); a slender
  cantilever solved on the GPU without falling back, and the same run as the
  CPU's; the fp32-floor systems (cube, 10:1 and 20:1 beams, frozen skin)
  refined to 1e-5 against an fp64 factorization, with the element energies;
  the SpMV leaving a rigid translation exactly unloaded. Hidden:
  `[fp32floor]` probes (residual floor vs slenderness, what zeroes the
  solve, condition numbers, penalty ramp, fp32 Ke as an operator),
  `[refinebench]` (SIMP GPU vs CPU vs exact on slender parts),
  `[mg0gate]`, `[mgscale]`, benches.
- `tests/topo_smoke.py`: start → poll → done, STL export, an unchanged
  study reused in under a second, cancel keeping a best-so-far result, one
  run at a time. Skips without Netgen. Reloading a
  saved result is covered by `test_topopt.cpp`.
- GUI: `sim/topology_holds_its_volume_target`.

## Not done

- Stress-constrained optimization, multiple load cases, manufacturing
  constraints (draw direction, symmetry, extrusion), frequency objectives.
- Assembly-level optimization and keep-out bodies.
- Density filtering with Heaviside projection (sharper edges and true
  minimum-member-size control); the current sensitivity filter leaves a grey
  band at member boundaries that the threshold and STL smoothing clean up.
- Automatic parametric reconstruction.
- The multigrid cycle costs ~20 ms an iteration on the 33k-tet yoke, most of
  it the CPU coarse solve between submits; that, not the fp64 refinement, is
  now the solve's cost.
