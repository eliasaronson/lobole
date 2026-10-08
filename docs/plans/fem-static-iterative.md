# Static studies on an iterative solver (CPU CG, then the GPU)

## The problem

A static study factorises K with Eigen's `SimplicialLDLT`
(`fem::SolveCases`, src/core/fem/FemSolver.cpp). Fill-in grows fast on a 3D
tet10 mesh, so time and memory go up steeply with element count:

- The planetary sun (generators/planetary, `--study`) at 61k tets (280k
  DOF) did not finish in 10 minutes, and one instance held 9 GB doing it.
- The run cannot be cancelled. The factorisation has no interruption
  points, so `cancel_study` and even SIGTERM wait it out.

A tooth-root stress needs elements a fraction of a millimetre across, so
studies that matter are exactly the ones that do not finish.

Topology optimisation already solves the same tet10 operator iteratively:
CPU CG with an incomplete Cholesky preconditioner, or the GPU's matrix-free
PCG with multigrid (`gpu::TopoGpuSolver`, docs/topopt/README.md#the-gpu-solver),
behind the seam `fem::TopoLinearSolver` (src/core/fem/FemTopo.h). Static
studies cannot use either today.

## The shape of the fix

Above a DOF threshold, `SolveCases` solves iteratively. Below it, LDLT stays:
small studies are fast already, and LDLT is the oracle the others are tested
against. Every path keeps LDLT's contract: the residual
`‖Ku − F‖ ≤ 1e-6 ‖F‖` is checked, and a singular K is refused as
under-constrained.

### Stage 1: CPU CG

- Split `SolveCases` into its three parts: assembling F (face loads, gravity),
  the linear solve, and recovering results (expanding u, nodal stresses, von
  Mises). Both paths then share the first and the last.
- Iterative solve: `ConjugateGradient<…, Lower|Upper,
  IncompleteCholesky<double>>` on the K already assembled.
  - Tolerance 1e-8 relative, an explicit iteration cap.
  - `info() == Success` is checked, then the residual as above. Today's topology
    copy checks neither (FemTopo.cpp:1102).
- Solved in chunks (`setMaxIterations` and `solveWithGuess`), with a cancel
  callback between chunks, so `cancel_study` stops a static run for the first
  time. The callback is threaded through `SolvePreparedStudy` from the job's
  `cancel` flag.
- The solver used is reported per run (`study_status.solver`) instead of
  the hard-coded `CpuSolverName`.

### Stage 2: the GPU

- `TopoLinearSolver::Setup` takes one material per mesh domain. Each tet
  packs its own D (FemTopo.h, TopoGpuSolver.cpp `Setup`). A scale of
  E_d / E0 is not enough, since ν differs between domains.
- `Solve` takes a tolerance and reports the residual it reached. A static
  caller asks for 1e-8 and refuses an unconverged answer (topology keeps
  accepting one). The warm start is reset per load case.
- A cancel hook between refinement steps.
- The app builds the solver for static studies the same way it does for
  topology: the `HAVE_FEM_GPU` and `TopoGpuSolver::Create` block in
  `ui::StartTopologyRun`, factored into a helper that `StartStudyRun`,
  `StartAssemblyStudyRun` and `StartTopologyCheck` share. It is passed into
  `SolveCases` as an optional `TopoLinearSolver*`, so `designer_core` stays
  GPU-free. `DESIGNER_TOPO_GPU=0` forces the CPU, as it does for topology.

## Files

- `src/core/fem/FemSolver.{h,cpp}`: the split, the iterative branch, the
  cancel callback, the solver name.
- `src/core/fem/FemStudy.{h,cpp}`: thread the solver and cancel through
  `SolvePreparedStudy`; report the solver used.
- `src/core/fem/FemTopo.{h,cpp}`: the seam's per-domain materials,
  tolerance and residual; topology's call adapted.
- `src/femgpu/TopoGpuSolver.{h,cpp}`: per-domain D, strict mode,
  per-case reset, cancel.
- `src/ui/SimulationActions.cpp`: the shared solver helper; the solver name
  from the run, not hard-coded.
- `tests/test_fem.cpp`, `tests/test_femgpu.cpp`.
- docs/simulation/README.md ("The solver"; the cancel wording),
  docs/topopt/README.md ("The GPU solver"), the `run_study` and
  `study_status` rows if what they report changes.

## Verification

- The new paths against LDLT on the same model, to 1e-5 relative in
  displacement and von Mises: a slender cantilever; a two-material bonded
  block with different ν; several load cases on one model; gravity. The
  existing beam-theory tests (3-12 %) cannot see solver error.
- A singular K (an under-constrained model) is still refused on every path.
- `cancel_study` stops a large static run within a second, and nothing is
  stored.
- `femgpu_tests`: the static solve against `DirectOracle`. It self-skips
  without a device, as the suite does.
- The planetary sun at the mesh it actually wants (0.4 module) solves in
  well under a minute, and its stresses match a coarser LDLT run within the
  mesh's own convergence.
