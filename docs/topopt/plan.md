# Topology optimization (generative) — design

> Status: **design, for sign-off.** Nothing is built. v1 = SIMP compliance
> minimization on a single part, run as a new kind of Study on top of the
> shipped FEM stack (Netgen tet10 mesh + Eigen solver), producing a persisted
> density-field result, an iso-surface preview body, smoothed STL export, and
> a "shape guide" ghost for re-modeling. Parametric B-Rep *reconstruction* of
> the organic result is explicitly out of scope (see "The parametric
> question" — the honest industry answer is re-model-over-a-ghost, which v1
> supports; auto-reconstruction is a research tar pit we are not entering).

## Why (and why now)

The FEM loop closed "does this part survive?" — topology optimization closes
"what *should* this part be?". The LLM angle is again the differentiator:
"here's the design envelope, these faces bolt down, this face carries 2 kN —
give me the stiffest shape at 30% of the mass" is a single headless
conversation: `add_study type=topology` → set loads → `run_study` → export
STL / re-model against the ghost. No mainstream CAD exposes that loop as
text.

Why now: TO is ~85% infrastructure we already shipped. It needs a tet mesh
(FEM1), a linear-static solve per iteration (FEM2), face BCs via `ElementRef`
(FEM1), scalar-field display (FEM3 `FieldRenderer`), study CRUD/RPC/MCP
plumbing (FEM1–4), and stale-tracking (`StudyRuntime`). The genuinely new
code is the optimization loop itself: per-element stiffness scaling, a
sensitivity filter, and an optimality-criteria update — well-trodden numerics
(the "99-line topopt" lineage), not research.

Honest cost statement: ~1.5× the FEM-solver phase. The loop is small; the
cost is in the surrounding reality — long runs need a worker thread +
progress + cancel (the app's first genuinely long-running operation),
results must be persisted (a first for derived data), and iso-surface
extraction + smoothing + export is its own small pipeline.

## Method survey → decision: SIMP + optimality criteria

- **SIMP (density-based)** — each element gets a density ρ ∈ [ρmin, 1];
  stiffness scales as ρᵖ (p=3 penalizes intermediate densities toward 0/1);
  minimize compliance subject to a volume fraction; gradient-based update.
  **Chosen**: it reuses the existing mesh + solver verbatim, sensitivities
  are nearly free for compliance (self-adjoint), and it is the method behind
  virtually every production tool (Altair, Ansys, Fusion).
- **BESO/ESO (evolutionary)** — discrete element add/remove. Simpler but
  slower to converge and theoretically shakier. Kept as a fallback only.
- **Level-set** — crisp boundaries by construction, but needs velocity
  extension + reinitialization machinery and is much harder to make robust
  on unstructured tets. Not v1.
- **Homogenization / ground-structure** — academic / truss-specific. No.

The SIMP loop per iteration: assemble K with per-element scale ρᵖ → solve
Ku=f (existing solver) → per-element compliance sensitivity
`dC/dρₑ = −p·ρₑᵖ⁻¹·uₑᵀKₑ⁰uₑ` (the element strain energy, already computed
during stress recovery) → **density filter** (radius R over element
centroids — enforces a minimum member size and mesh-independence; the one
genuinely fiddly piece on unstructured tets, solved with a uniform-grid hash
over centroids) → **OC update** (bisect the Lagrange multiplier on the
volume constraint; move limit 0.2, damping η=0.5; ~30 lines) → repeat until
max density change < 1% or the iteration cap.

Performance note that makes minutes tolerable: K's sparsity pattern never
changes across iterations — `SimplicialLDLT::analyzePattern` once, refactor
values each iteration. Tet10 is finer than the linear elements classic SIMP
uses, which is fine (better, even — less checkerboarding; the filter is
still required for minimum member size).

## TO0 spike findings (gate: PASS)

The TO0 spike (`src/core/FemTopo.{h,cpp}`, `tests/test_topopt.cpp`) built the
SIMP+OC loop on the shipped tet10 solver and ran it on a structured-tet
cantilever. **Verdict: PASS** — the loop is stable and correct: the OC
volume constraint lands the target to <0.1%, compliance drops ~6× (3947 →
660 on the gate problem), the fixed/loaded skin stays solid, and the
thresholded density is a recognizable cantilever truss (solid top+bottom
chords, webbed interior). Four findings refine the design:

1. **Filter: sensitivity filtering for v1, not density filtering.** A density
   filter blurs the density field, leaving a wide gray transition band that
   coarse meshes can't sharpen (measured 22–40% gray). Sensitivity filtering
   (Sigmund) keeps the physical density equal to the design variable — the
   field goes crisply 0/1, the frozen skin is exactly 1, and the volume stays
   linear in x (O(n) OC bisection). **Density filter + Heaviside projection**
   (the three-field robust formulation) is the production-grade path to true
   minimum-member-size control and razor-sharp edges — deferred to TO1.
2. **Gravity is excluded from the TO objective.** Self-weight is a
   design-dependent load (F depends on ρ), which adds a dF/dρ sensitivity term
   and is a known low-density SIMP failure mode. TO optimizes the study's
   fixed face loads only; gravity stays an FEM-only feature.
3. **The direct solver is the wall, and it sets the resolution ceiling.**
   Each `SimplicialLDLT` factorization is ~0.4 s at ~3k tets and grows
   super-linearly (~2.8 s at ~7k); a converged run is 60–80 of them.
   Crisp results want finer meshes, but the direct solver makes that
   minutes-to-slow. The **scaling path** (its own later workstream, not TO1):
   a matrix-free Conjugate-Gradient solver — cheapest first as CPU CG with
   warm-starting (reuse the previous iteration's u; K barely changes between
   SIMP steps), then **Vulkan compute** (element-wise K·v in a compute shader,
   the standard high-resolution-GPU-TO approach) with a multigrid
   preconditioner for the Emin-induced ill-conditioning. The app already has
   the Vulkan device/queue/shader plumbing. This is what makes fine-mesh,
   production-quality TO interactive; the CPU direct solver is fine for TO1's
   modest meshes on the worker thread.
4. **Element math was refactored, not forked.** The tet10 shape functions,
   Gauss rules, elasticity matrix, and the DOF-map / face-load assembly moved
   from `FemSolver.cpp`'s anonymous namespace into `core/FemSolverInternal.h`
   (`fem::internal`), shared verbatim by the solver and the TO loop — the
   existing `[fem]` analytic benchmarks guard that the motion changed nothing.

## What v1 is — and is not

**v1: single-part compliance minimization.**

- Design domain = **one body** (the user models a blocky design envelope;
  modeled holes/cutouts are naturally keep-out). Loads/fixtures = the
  existing Study loads on faces.
- Constraint = volume fraction (target mass kept, expression-driven).
  Objective = compliance (stiffness) under the study's static loads +
  optional gravity.
- Non-design (frozen at ρ=1): every element touching a loaded or fixed
  face, plus user-added **keep faces** (so bolt bosses and bearing seats
  survive), each frozen within the filter radius of that face.
- Result = per-element density field, **persisted in the document**
  (unlike FEM results — see below), stale-badged on regen, never recomputed
  automatically.
- Display = iso-surface preview body (marching tetrahedra at an adjustable
  threshold, volume-corrected default) + the density colormap via
  `FieldRenderer`.
- Export = smoothed STL of the iso-surface (Taubin smoothing — no shrink).
- **Shape guide**: show the optimized surface as a translucent ghost in
  normal modeling mode (the in-context-ghost pattern) so the user re-models
  clean parametric geometry over it. This *is* the "make it parametric"
  workflow (below).

**Explicitly deferred (decisions, not gaps):**

- **Automatic B-Rep reconstruction** — see next section. Not v1, likely not
  ever in automatic form.
- **Stress-constrained TO** — compliance is the robust v1 objective; stress
  constraints need adjoint sensitivities + aggregation (p-norm) and are
  famously touchy. The shipped FEM can always *check* stresses on the
  re-modeled part afterward, which is the engineering-honest workflow
  anyway.
- **Multiple load cases** — weighted-sum compliance over cases is a small
  extension (one solve per case per iteration); deferred to keep v1's
  runtime and UI simple.
- **Manufacturing constraints** (draw direction, symmetry planes, extrusion
  constraint), **frequency objectives**, **assembly-level TO**, **keep-out
  bodies** (vs. modeled holes) — all later, on top of a proven loop.
- **Lattice / parameter (size) optimization** — a different, parametric-by-
  construction track (optimize existing driving dimensions under FEM
  constraints). Arguably a better strategic fit for a parametric CAD and
  cheaper than TO, but it is its own design doc if wanted.

## The parametric question (answered up front)

TO output is an organic blob; "make it parametric" spans three tiers:
faceted dumb solid (cheap, not editable), fitted analytic B-Rep
(scan-to-CAD, a commercial product category by itself), and feature-tree
reconstruction (research-grade, effectively unsolved). Commercial generative
tools don't auto-reconstruct either — the engineer re-models over the result.

**Decision: we ship the workflow, not the reconstruction.** The optimized
surface becomes a ghost shape-guide in modeling mode; the user sketches and
extrudes clean parametric geometry against it; the existing FEM study then
verifies the re-modeled part. That is the actual industrial loop, it costs
days not months, and it keeps every downstream model fully parametric. STL
export covers the print-it-directly case.

## The model: a new AnalysisType on the existing Study

**Decision: `AnalysisType::Topology` on `fem::Study`** — not a new object,
not a Feature. Same reasoning as FEM (consumes finished bodies, produces no
regen geometry) plus a stronger one: a TO run is minutes-long and
mesh-dependent, so it must never sit in the regeneration path. Reusing Study
inherits CRUD commands, load editing, face picking, serialization shape,
staleness, and the Studies panel for free.

```
Study {  // existing fields: id, name, type, meshSize(+expr), refinements,
         // loads, gravity — all reused as-is
  // Topology only:
  double volumeFraction = 0.3;   std::string volumeFractionExpr;
  double filterRadius = 0.0;     std::string filterRadiusExpr;  // mm; 0 = 2.5x mesh size
  int maxIterations = 60;
  std::vector<ElementRef> keepFaces;  // frozen solid, beyond auto-frozen BC faces
}
```

Validation at run: exactly one body loaded (v1 is single-body), at least one
fixed support, volume fraction in (0,1), material has stiffness data — all
up-front errors on `study.error`, the FEM pattern.

## Results: persisted, unlike FEM — the one rule change

FEM results are derived-and-never-serialized because recompute is seconds.
A TO result is minutes of compute **and** tied to a mesh that is not stable
across Netgen versions/machines — recompute does not reproduce it.

**Decision: the TO result is document data, serialized.** Format bump
(**v33**), additive per F3:

```
parts/bracket/studies/
  12-topo-1.json            # the Study definition (existing shape + new fields)
  12-topo-1.result.json     # present only after a successful run
```

`result.json` = `{ solved_at_version, iterations, compliance_history,
volume_fraction_achieved, mesh: {nodes, tets}, density: [per-tet] }` with
flat numeric arrays (a ~30k-tet result is a few MB of JSON; it changes only
on an explicit re-run, so git-checkpoint churn is bounded and honest).
Loaded back into the same `StudyRuntime` cache slot the FEM results use —
one staleness/epoch/panel mechanism for both; `solvedAtVersion` vs.
`Document::Version()` gives the stale badge across sessions. Stale results
are kept and displayed (they are still the best available shape), just
badged.

Golden + round-trip per the migration policy; the golden result file is a
tiny hand-made mesh (4 tets), not a Netgen product — goldens must not
depend on mesher stability.

## The run: the app's first long-running operation

A TO run (60 × factorize+solve) is minutes. A synchronous Run button freezes
the GUI; a synchronous RPC times out clients.

**Decision: run on a worker thread over an immutable snapshot.** At launch,
the main thread evaluates expressions, resolves refs, meshes, and builds a
self-contained `TopoInput` (mesh + material + fixed nodes + loads + TO
params — plain `fem` data, no Document access). The thread runs the loop,
publishing `{iteration, compliance, maxChange}` into an atomic progress
struct and checking a cancel flag each iteration. The main thread polls each
frame; on completion it stores + serializes the result (main thread owns the
Document, so the threading surface is exactly one join point). Cancel keeps
the best-so-far densities as a valid (badged "cancelled at iter N") result.

Surface: `run_study` on a topology study returns `{started: true}`
immediately; new `study_status` → `{running, iteration, max_iterations,
compliance, cancelled}`; new `cancel_study`. The GUI Run button becomes a
progress bar + Cancel while running. One run at a time (a second `run_study`
while running is an error). Headless smokes use a coarse mesh + low cap so
the poll loop finishes in seconds.

## Display & export

- **Iso-surface preview** (the primary view): average densities to nodes,
  marching-tetrahedra at threshold t over the tet corners (linear tets —
  midside nodes ignored for extraction), default t chosen by bisection so
  the enclosed volume matches the achieved fraction. Rendered as a normal
  grey body through `MeshRenderer` (facet normals) with the design body
  hidden — the `hiddenMeshes` swap the field overlay already does. A
  threshold slider re-extracts (cheap, ~ms).
- **Density colormap** as an alternate field: reuse `FieldRenderer`
  verbatim (`FieldVertex{position, displacement=0, scalar=ρ}` on the
  boundary tris + iso-surface). The Simulation-mode field selector grows a
  "Density" entry for topology studies.
- **STL export**: iso-surface → 3–5 Taubin iterations (λ/μ pair — smooths
  the staircase without shrinking) → the existing STL writer path. Exposed
  as `export_topo_stl {study, path, threshold?}`.
- **Shape guide ghost**: outside Simulation mode, an "Overlay result" toggle
  renders the extracted surface translucent in normal modeling mode
  (in-context-ghost rendering pattern), so sketches/extrudes are built
  against it. This is deliberately dumb: no snapping to the blob (nothing
  stable to snap to), just visual reference.

## Surface: panel, RPCs, MCP

Panel (Studies section, Simulation mode): a topology study shows volume
fraction, filter radius, max iterations, keep-face list (face picking, the
load-picking flow), Run/progress/Cancel, result summary (iterations,
compliance curve sparkline, achieved fraction, stale/cancelled badge),
threshold slider, field selector, Export STL button, ghost toggle.

RPCs (in `remote::HandleSimulationRpc` — the domain module, per the split):

- `set_study {study, type: "topology", volume_fraction?, filter_radius?,
  max_iterations?}` (extends the existing `set_study`)
- `add_keep_face {study, face:{feature_id, element}}` / `remove_keep_face`
- `run_study` (existing name; topology ⇒ async semantics above),
  `study_status {study}`, `cancel_study {study}`
- `study_results {study}` → `{iterations, compliance, volume_fraction,
  stale, cancelled}`
- `set_result_display {study, field: density|none, threshold?}` +
  `set_topo_ghost {study, on}` — LLM screenshot of the optimized shape =
  two calls, the FEM/explode pattern
- `export_topo_stl {study, path, threshold?}`

MCP mirrors for each, `@mcp.tool` per convention. LLM parity end-to-end:
model envelope → loads → `run_study` → poll `study_status` → screenshot /
export — no GUI required.

Tests: Catch2 `test_topopt.cpp` (filter weights on a hand mesh, OC update
invariants, full loop on a coarse cantilever — see gate), serialization
round-trip + golden (v33), `topo_smoke.py` headless (coarse envelope, fix +
load, run with cap≈15, poll to completion, assert volume fraction ± tol and
compliance decreased, export STL, re-open project and see the persisted
result). Self-skips without Netgen, like `fem_smoke`.

## Decisions for sign-off

1. **Method = SIMP + OC** (p=3, density filter over element centroids,
   move-limited OC update), reusing the FEM mesh + solver; BESO is the
   fallback, level-set rejected for v1.
2. **v1 scope**: single-body design envelope, compliance objective, volume-
   fraction constraint, keep-faces + auto-frozen BC skin; stress
   constraints, multi-load-case, manufacturing constraints, assembly TO all
   deferred.
3. **No automatic parametric reconstruction — ship the re-model workflow
   instead**: iso-surface ghost shape-guide in modeling mode + smoothed STL
   export. Tiered reconstruction documented as out of scope.
4. **Model = `AnalysisType::Topology` on the existing `fem::Study`** (not a
   Feature, not a new object): + volumeFraction, filterRadius,
   maxIterations, keepFaces; expressions per the text+value convention.
5. **TO results are serialized** (`studies/<id>.result.json`, flat arrays,
   **format v33**) — the deliberate exception to the derived-never-stored
   rule, justified by minutes-cost + mesh-nonreproducibility; loaded into
   the same `StudyRuntime` cache; stale-badged, never auto-rerun. Goldens
   use a hand-made mesh, never Netgen output.
6. **Long runs go on a worker thread** over an immutable `TopoInput`
   snapshot with atomic progress + cancel; Document touched only at the
   main-thread join. `run_study` async for topology + `study_status` +
   `cancel_study`. One run at a time.
7. **Solver hook, not fork**: the tet10 element math + system-assembly
   helpers live in `core/FemSolverInternal.h` (`fem::internal`), shared by the
   static solver and `fem::TopologyOptimize`; the TO loop assembles its own
   penalized K per iteration and reuses the symbolic factorization
   (`analyzePattern` once). (Implemented in TO0.)
8. **Display = marching-tets iso-surface as a MeshRenderer body**
   (threshold slider, volume-matched default) + density via the existing
   `FieldRenderer`; export = Taubin-smoothed STL.

## Phasing

0. **TO0 — spike (gate): DONE — PASS.** Solver hooks (element scale + strain
   energy via `core/FemSolverInternal.h`), centroid-grid filter, OC loop in
   `core/FemTopo.{h,cpp}`, driven by structured-tet box cantilevers in
   `tests/test_topopt.cpp` (deterministic, Netgen-free) + a Netgen cross-check.
   The loop converges under the cap (~65 iters), lands the volume fraction to
   <0.1%, drops compliance ~6×, keeps the fixed/loaded skin solid, and the
   thresholded density is a recognizable cantilever truss. Per-iteration wall
   time measured (~0.4 s at ~3k tets, ~2.8 s at ~7k — the direct solver is the
   scaling wall; see finding 3). **Two design changes the spike settled**
   (above): sensitivity filtering (not density) for v1 crispness, and gravity
   excluded from the objective. Grayness landed at ~30–40% on the coarse
   structured-tet mesh (filter-band + Kuhn-decomposition sensitivity variation)
   rather than the <15% originally hoped — razor-sharp black/white is a TO1
   Heaviside-projection / finer-mesh concern, not a viability blocker; the
   binarized truss is correct. SIMP is viable on tet10 — GO.
1. **TO1 — model + persistence: DONE.** `AnalysisType::Topology` +
   volumeFraction/filterRadius(+expr)/maxIterations/keepFaces on `fem::Study`
   (reusing `ModifyStudyCommand`'s before/after-JSON undo). `fem::RunTopology`
   resolves the single loaded body + fixed/force/keep faces and drives
   `TopologyOptimize`; `RunStudyAndCache` dispatches on type; `run_study` is
   synchronous for now. Serialization v33: definition fields (only-when-
   topology, so lower versions stay byte-identical) + the **persisted result**
   (`studies/<id>.result.json`, flat mesh+density+summary arrays; loaded results
   stamped with the settled post-load version so they read current, not stale).
   RPCs: `set_study` topology params, `add_keep_face`/`remove_keep_face`,
   `run_study`/`study_results` topology summaries, `list_studies` type.
   Tests: definition + result round-trip, RunTopology end-to-end (Netgen),
   dir-format result persistence, golden `v33-topology`. 395/395 ctest green.
   (MCP mirrors + async run land in TO4/TO2.)
2. **TO2 — the worker thread.** Snapshot/launch/poll/cancel machinery,
   async `run_study` + `study_status` + `cancel_study`, result stored +
   serialized at join, `topo_smoke.py`.
3. **TO3 — display.** Marching-tets extraction + threshold slider +
   MeshRenderer preview body + density field entry + panel section
   (progress bar, Cancel, summary, badges) + `set_result_display`
   extension. Verified via dev-watch.
4. **TO4 — export + guide + MCP.** Taubin smoothing + `export_topo_stl`;
   the modeling-mode ghost + `set_topo_ghost`; MCP mirrors; docs
   (`docs/topopt/guide.md`).

Order rationale: 0 gates the whole workstream on the only real numerical
risk for a day's work; 1 before 2 so the loop is testable synchronously
before threading exists; display after the headless loop per the
headless-core rule; export/ghost last because they consume the extraction
built in 3.
