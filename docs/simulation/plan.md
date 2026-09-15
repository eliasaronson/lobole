# Simulation — FEM structural analysis — design

> Status: **v1 + v2 shipped.** v1 = single-part linear static (FEM0–FEM4:
> headless mesh+solve loop, Simulation-mode GUI, MCP, `fem_smoke`). **v2 adds
> modal analysis and gravity/self-weight** (Spectra eigensolver, consistent mass
> matrix, `AnalysisType` static|modal on the Study, format **v32**, natural-
> frequency list + mode-shape overlay in the panel, `set_study
> type/num_modes/gravity` + modal `run_study`/`set_result_display` RPCs+MCP).
> Validated: cantilever f₁ 0.18% vs Euler-Bernoulli, self-weight 0.03% vs beam
> theory. v2 requires a fixed support (free-free deferred). Still deferred:
> assembly/contact, thermal, buckling, nonlinear, VTU export.
> Two facts the FEM0 spike settled, corrected throughout this doc:
> the studies format bump is **v29** (v28 was already taken by C2's
> unit-suffixed expressions), and the FEM code lives as flat `src/core/Fem*.cpp`
> files in a `namespace fem` (matching the flat `src/core/` house style) rather
> than a `src/core/fem/` subdirectory. FEM0 also proved the Netgen path: the
> C++ `OCCGeometry` interface (the C nglib API can't attribute surface elements
> to OCC faces), second order via an explicit `MakeSecondOrder()`, and the
> nixpkgs `netgen` links in-process against our OCC 7.9.3 — no fallback needed.
>
> The roadmap has listed "CAM, simulation/FEA
> — out of scope" since day one; this doc is a conscious reversal of half of
> that line (CAM stays out). What changed: the prerequisites FEM usually
> founders on in a parametric CAD are now *shipped* — stable face references
> (toponaming T1–T5), regen-aware staleness (A4 phase 1), per-body materials
> with physical properties started (density, I2), headless RPC testing, and
> Eigen already in the tree. The genuinely new problems are two: a tet mesher
> (a dependency, not code) and a linear-static solver (smaller than it
> sounds). Everything else is plumbing this codebase already has patterns for.

## Why (and why now)

The LLM angle is the differentiator, same as measure/mass-properties were:
"design a bracket, **run a study, report max von Mises vs. yield**" closes
the text-to-CAD verification loop for *function*, not just form. An assistant
that can iterate geometry against a stress number — headless, no screenshots
needed — is something no mainstream CAD exposes cleanly. Every piece of the
v1 below works through the same RPC surface humans use, per the house rule.

Honest cost statement: this is an A2+A3-sized workstream (the solver core is
the largest single phase), not an I2-sized one. The phasing is arranged so
the headless solve loop ships in phase 2, before any GUI exists.

## What v1 is — and is not

**v1 (this doc): single-part linear static analysis.**

- Tetrahedral meshing of part bodies (Netgen), global + per-face element size.
- Isotropic linear-elastic materials (Young's modulus, Poisson ratio, yield
  strength added to the material library).
- Boundary conditions on faces via stable `ElementRef`s: **fixed** support,
  **force** (total vector, spread as uniform traction), **pressure** (along
  face normal). Magnitudes expression-driven from birth.
- Second-order tetrahedra (tet10), in-house assembly + sparse direct solve on
  Eigen.
- Results: displacement field and von Mises stress, rendered as a colormapped,
  optionally deformed overlay; max values + location in a panel and over RPC.

**Explicitly deferred (decisions, not gaps):**

- **Assembly studies / contact** — bonded ties and contact are a different
  tier of difficulty (constraint equations or nonlinear iteration) and drag
  in per-occurrence meshing. Nothing in v1 precludes them; the Study model
  deliberately lives per-document so an assembly-level study kind can arrive
  later without reshaping this one.

  **Field note, 2026-09-15 (panel modes of the smart speaker,
  generators/smart_speaker/modal.py).** The deferral holds up: one panel at a
  time answered the question that was asked, because a radiating panel's
  modes are a panel property. Two things bit, and both are about BOUNDARY
  CONDITIONS rather than about solving a whole assembly:

  - a study on a part with several bodies reads as though it might couple
    them, and does not — one eigenproblem per body. The per-body `bodies`
    array in `study_results` is the only thing that says so, which is why
    that row now spells it out.
  - **a brace that BUTTS against a panel cannot be used as a support.** The
    cabinet's ring brace lands on a continuous inner face, so there is no
    face to clamp, and the one brace whose effect matters most (it crosses
    the baffle's fundamental antinode) is the one that could not be
    measured. The divider could, only because it happens to sit in a 4 mm
    dado. The assembly KNOWS these parts touch — `asm.check` reports it —
    and the study cannot use it.

  So the cheap half of "assembly studies" is not the solver: it is being
  able to say "fix this panel where that part touches it", which needs an
  imprint of the contact patch, not constraint equations. Worth separating
  from the hard half when this is picked up.

  **P0 answered, 2026-09-15** (`tests/test_fem.cpp`, "a mesh spans two solids
  only when they SHARE the face"). The technique works and the expensive
  branch is not needed:

  - a **general fuse** imprints the shared face, KEEPS both solids (so they
    can carry different materials later), and Netgen then meshes that face
    once — one connected tet component, no doubled nodes on the interface.
    Load crosses it. The existing solver would analyse a bonded assembly
    unchanged, because it takes a mesh and not an assembly. No tied
    constraint equations.
  - the control matters: an UNGLUED compound of the same two solids meshes
    as two components with every interface node doubled, so the check has
    teeth.
  - **`SetFuzzyValue` is not optional.** Parts are placed at float32, so
    nominally coincident faces sit microns apart; a strict boolean finds
    nothing between them and the assembly silently meshes as loose pieces —
    which LOOKS like a working study. 1e-3 imprints a 3 um gap.

  Two things the real cabinet added that boxes could not:

  - **granularity is a design decision, not an inference.** Fusing all 40
    placed solids passed the boolean and then failed to mesh, on 2 faces of
    1204 — a 19 mm2 cylinder where the port tube's line-to-line d105 fit
    meets its flare, which fusing turns into slivers. T-nuts, drivers and a
    press fit are not glue. Restricted to the 18 structural solids the fuse
    took 1.1 s and meshed clean.
  - **scale is the real ceiling, and it is nearer than estimated.** That
    structural subset at a 40 mm size — coarse — came to 524,405 tets and
    837,279 nodes: about 2.5M DOF, 73 s just to mesh. Not the global size
    doing it (a uniform 40 mm fill of this volume would be ~1,100 tets) but
    16 mm panels capping the local size and several hundred fastener holes
    refining it further. A whole-cabinet modal solve therefore needs
    DEFEATURING (suppress the fastener holes for analysis) before it needs a
    better eigensolver, and per-panel studies keep their place regardless.
- **Modal analysis** — the natural FEM2: mass matrix assembly is cheap once
  stiffness assembly exists, and density is already in the library. Wants a
  sparse eigensolver (Spectra is header-only BSD, vendors like PlaneGCS).
  Deferred only to keep v1 small.
- **Gravity / body loads** — easy (density × volume), deferred with modal
  since both need the density→solver-units path.
- **Nonlinear (plasticity, large deformation), thermal, buckling, fatigue** —
  out of scope. If ever wanted, the documented escape hatch is driving
  CalculiX as a subprocess (write `.inp`, parse `.frd`, FreeCAD's approach)
  rather than growing the in-house solver past linear.
- **Shell/beam elements, adaptive refinement** — solid tets only; refinement
  is manual (per-face size) in v1.
- **Result export (VTU for ParaView)** — cheap and useful, but a nicety;
  listed under later.

## 1. Meshing: Netgen, linked, optional

**Decision: link Netgen (nglib + its OCC interface), gated like PlaneGCS.**

- License: LGPL — same standing as the vendored PlaneGCS (`THIRD_PARTY.md`).
  Gmsh is GPL (contaminating if linked), TetGen AGPL; Netgen is the only
  serious tet mesher with a compatible license *and* a native OCC interface
  (it meshes a `TopoDS_Shape` directly — no STEP round-trip, no unit loss).
- Availability verified 2026-07-13: nixpkgs `netgen` 6.2.2605 is built **with
  `opencascade-occt` in its buildInputs** — the same package `shell.nix`
  already uses, so from one nixpkgs pin there is a single OCC in the process.
  One line in `shell.nix`, a `find_package`/`find_library` block in CMake.
- Gating mirrors `HAVE_PLANEGCS` exactly (`CMakeLists.txt:206-230`): absent
  Netgen, the app builds and runs; `run_study` fails with a clear
  "built without FEM support" error. FEM code compiles only the solver-side
  tests when the mesher is missing.
- Mesh settings per study: a global max element size (mm, expression-driven;
  0 = auto from bbox diagonal, like tessellation's adaptive deflection at
  `CADKernel.cpp:370`) plus optional per-face refinements keyed by
  `ElementRef`. Netgen supports local mesh size natively.
- Second order from the mesher (`SecondOrder()`), so midside nodes follow
  the true surface curvature rather than being interpolated flat.

**FEM0 is a spike with a decision gate** (see phasing): nglib's API is old
and quirky, and the nixpkgs build's OCC interface needs proving against our
OCC version *before* anything else is built on it. Fallback if it fails:
build Netgen ourselves via a nix override, or shell out to Gmsh as a
subprocess (GPL is fine across a process boundary) — a decision to make with
the spike's evidence, not now.

**Known reality check:** meshing fails or produces slivers on real CAD
(thin fillets, short edges). v1 answers: per-face size control, mesh
statistics surfaced (node/element count, worst element quality), and mesher
errors reported as study errors naming the offending face where Netgen says
so. Defeaturing tools are not FEM's job (suppression already exists for
that — suppress the cosmetic fillets, then mesh).

## 2. The solver: in-house, on Eigen

**Decision: write the linear-static solver in-core (tet10 isotropic
elasticity on Eigen), not CalculiX.**

Rationale: linear static on tets is a well-trodden ~2–4k lines — element
stiffness (tet10, 4-point Gauss), triplet assembly into a sparse K, Dirichlet
elimination, `SimplicialLDLT` solve, stress recovery, von Mises. Eigen is
already a dependency. In-process means: headless-testable with Catch2 like
every other core capability, no Fortran toolchain, no `.inp`/`.frd` file
formats, deterministic error surfaces ("body has loads but no fixed support"
detected *before* a singular factorization, not after). CalculiX would trade
all that for nonlinear capability v1 doesn't want.

Solver contract (all in flat `src/core/Fem*.cpp`, `namespace fem`, GUI-free):

- **Units: the mm–N–MPa system** (consistent with the document's mm
  convention): lengths/displacements mm, forces N, pressures/stresses/moduli
  MPa. The material library stores E in GPa and yield in MPa for
  human-friendly numbers; the solver converts. Density (kg/m³) is unused
  until gravity/modal.
- **DOF and constraints:** fixed faces zero all three DOF on their nodes
  (v1 has no partial constraints); constrained DOFs are eliminated (reduced
  system), not penalized.
- **Loads:** force = total vector distributed as uniform traction over the
  face's mesh facets (consistent nodal loads, not equal splitting — equal
  splitting is visibly wrong on tet10); pressure = traction along the
  outward face normal, positive = pushing.
- **Per-body solves:** bodies are independent (no contact); only bodies with
  at least one load or constraint are meshed and solved. A loaded body with
  no fixed face is a study error up front.
- **Recovery:** element stresses at integration points, volume-weighted
  averaging to nodes, von Mises per node. Reported: max von Mises (value +
  location), max displacement magnitude (value + location), per the
  measure-tool philosophy — the number an engineer actually asks for.
- **Failure is first-class:** singular factorization (insufficiently
  constrained despite the pre-check — e.g. a fixed face whose area meshed to
  nothing) sets the study's error string, same pattern as
  `Feature::SetError`.

**Testing policy: analytic benchmarks with tolerances, never golden meshes.**
Netgen output is not bit-stable across versions, so numeric golden files
would rot. `test_fem.cpp` instead asserts physics: uniaxial bar σ = F/A
(near-exact), cantilever tip deflection vs. Euler–Bernoulli (few % at tet10
with sensible mesh), thick-wall pressurized cylinder vs. Lamé. NAFEMS
benchmarks (LE1/LE10) can join later. These run headless in CI like every
core test.

## 3. The Study: a document-level object, results derived

**Decision: a `Study` is not a `Feature`.** Features are steps in the
regeneration history and can only reference what precedes them; a study
*consumes the finished bodies* and produces no geometry. It lives on
`Document` as a sibling list to the parameter table (SolidWorks and Onshape
reached the same shape). This also keeps rollback/reorder/suppression
semantics untouched.

```
Study { id, name,
        meshSize + meshSizeExpr,              // mm; 0 = auto
        refinements: [ { ElementRef face, size + sizeExpr } ],
        loads:       [ Load { id, type: Fixed|Force|Pressure,
                              ElementRef face,
                              magnitude + magnitudeExpr,   // N or MPa
                              direction /*vec3, Force only*/ } ] }
```

- Ids from the document's `m_nextId`, like features. Expression pairs follow
  the `VisitExprs` text+value convention and evaluate against
  `Document::Symbols()` — a load can be `"bolt_preload * 1.5"`.
- **Face references are `ElementRef`s** (`ElementMap.h:13`), resolved at
  mesh time exactly as `ShellFeature` resolves its faces
  (`ShellFeature.cpp:45-64`). A ref whose name vanished (face consumed by a
  later fillet) is a study error naming the load — resolution failure as a
  first-class signal, the toponaming contract.
- **Mesh and results are derived data, never serialized** — the same rule as
  element names and the BOM (format guarantee 3). They live in a transient
  per-study cache: `{ solvedAtVersion, FemMesh, FemResults }`. Reopening a
  project shows studies configured but unrun.
- **Staleness, not auto-solve:** results are stamped with
  `Document::Version()` at solve; any regeneration makes them stale (panel
  badge + `study_results` reports `stale: true`). Unlike the A4 assembly
  solve, re-solving is *not* automatic — meshing+solving can take seconds
  and must stay an explicit act (a button, an RPC call).

**Persistence — format bump (v29; v28 is C2's unit-suffixed expressions):** additive
per the F3 discipline, mirroring the assembly `mates/` pattern:

```
parts/bracket/
  index.json                    # + "studies": [ids]  (absent when none — prior version byte-identical)
  features/...
  studies/
    12-static-1.json            # {type:"Study", id, name, mesh_size, refinements, loads:[...]}
```

Golden + round-trip per the migration policy. Undo: study edits go through
Commands like everything else.

**Materials:** `MaterialProperties` gains `youngGPa`, `poisson`, `yieldMPa`
with the same 0-=-unknown convention as `density` (`CADMaterials.h:43`) —
steel/aluminum/etc. get real values, `DEFAULT` stays 0 and a study on an
unassigned body fails with "material has no stiffness data" rather than
guessing. No format impact (the library is code, not document data).

## 4. Results display: a new field pipeline, `Vertex3D` untouched

The mesh pipeline's `Vertex3D` is `{position, normal}` with color per
*instance* (`MeshRenderer.h:9,66`) — a result field needs per-*vertex* data,
and threading a scalar through the shared vertex format would tax every
existing pipeline for one overlay's benefit.

**Decision: a dedicated results renderer** (own pipeline + vertex format,
the `SketchRenderer`/`OutlineRenderer` precedent), drawing the tet mesh's
boundary triangles instead of the B-Rep tessellation:

```
FieldVertex { vec3 position;      // undeformed node position
              vec3 displacement;  // solved u, mm
              float scalar; }     // active field value, pre-normalized to [0,1]
```

- **Vertex shader** warps `position + deformScale * displacement`
  (push-constant scale) — the deformation slider re-renders without
  re-upload. **Fragment shader** maps the scalar through an analytic
  colormap (Turbo polynomial — no texture, colorblind-safer than jet).
- Shares the `UniformBufferObject` layout, so the **section clip plane works
  on result fields for free** — clipping into a stressed part is exactly the
  raster section payoff (std140 discipline per `MeshRenderer.h:32`).
- While a field is displayed, the body's normal instance hides and the field
  overlay replaces it; other bodies render normally. Legend (min/max +
  colormap bar) as an ImGui overlay; max-value marker in the viewport, like
  the CoG marker.
- Fields v1: von Mises, displacement magnitude. Not pickable, no outlines.
- Mesh preview (before/without solving) reuses the existing line pipeline:
  boundary-edge overlay, plus node/element/quality stats in the panel.

## 5. Surface: panel, RPCs, MCP

**Panel:** a Studies section in the Model dock (the BOM precedent): study
list (add/rename/delete), per-study mesh size + refinements, load list —
add-load flows through face picking exactly like the mate tool — Run button,
results summary (max σᵥₘ / max |u| / vs. material yield → factor of safety),
field selector + deform slider, stale badge.

**RPCs** (snake_case branch in `RemoteServer.cpp` + `@mcp.tool` mirrors,
per convention):

- `add_study {name?}` / `remove_study` / `list_studies`
- `set_study {study, mesh_size?}` /
  `add_refinement {study, face:{feature_id, element}, size}`
- `add_load {study, type, face:{feature_id, element}, magnitude?, direction?}`
  — a `face` may also be just the element name (as a bare string or
  `{element}`), and the body that HAS it is found: the name's own prefix is
  the feature that created the ELEMENT, which after a boolean is not the
  body's `created_by`
  / `remove_load` — `magnitude` is NEWTONS for a force (the total on that
  face, spread over its area) and MPa for a pressure, which pushes along the
  inward normal. Stated here as well as in the unit system above because
  every result scales linearly with it and a wrong unit is invisible in the
  answer.
- `run_study {study}` → `{nodes, elements, max_von_mises_mpa: {value, at},
  max_displacement_mm: {value, at}, stale: false}` (runs mesh+solve; errors
  surface here)
- `study_results {study}` → same summary + per-body breakdown + `stale`;
  refused with "study has not been run" when there is nothing to report,
  which is what a study reopened from a project always is (`has_results`
  and `stale` in `list_studies` say so without the refusal)
- `set_result_display {study?, field: von_mises|displacement|none,
  deform_scale}` — LLM screenshot of a stress plot = two calls, the
  `set_explode` pattern.

LLM parity end-to-end: set a variable, regen, `run_study`, read max stress
against yield, iterate — no GUI, no screenshots required (but available).

**Tests:** `test_fem.cpp` (mesher smoke + the analytic benchmark suite, §2),
serialization round-trip + golden on the version bump, `fem_smoke.py`
headless CTest following `bom_smoke.py` (build a bar, fix one end, load the
other, assert σ within tolerance over RPC). All headless-capable; CI-able
when Netgen is in the CI shell.

## Decisions for sign-off

1. **Scope reversal, scoped:** simulation enters the roadmap as a workstream;
   v1 = single-part linear static (tet10, isotropic, fixed/force/pressure on
   faces, von Mises + displacement). Assembly/contact, modal, thermal,
   nonlinear, shells all explicitly deferred; CalculiX-as-subprocess is the
   documented escape hatch if nonlinear is ever wanted. CAM stays out of
   scope.
2. **Meshing = Netgen, linked** (LGPL, native OCC interface, nixpkgs package
   already built against our OCC), gated `HAVE_NETGEN` on the `HAVE_PLANEGCS`
   template; app builds and runs without it. FEM0 spike gates the whole
   workstream on proving nglib+OCC in our environment; fallback (own Netgen
   build, or Gmsh subprocess) decided on the spike's evidence.
3. **Solver = in-house on Eigen** (already a dep): tet10, consistent nodal
   loads, Dirichlet elimination, `SimplicialLDLT`, nodal stress recovery.
   mm–N–MPa unit system; library stores E(GPa)/ν/yield(MPa) with the
   0-=-unknown density convention. Tested against analytic solutions with
   tolerances — never golden meshes (Netgen isn't bit-stable).
4. **Study = document-level object, not a Feature** (it consumes finished
   bodies, produces no geometry): sibling list to parameters, ids from
   `m_nextId`, loads/refinements hold `ElementRef`s with dangling-ref errors
   per the toponaming contract, magnitudes expression-driven via the
   text+value convention. **Format v29** (v28 = unit-suffixed expressions): `studies/` dir in
   the part `.dsgn`, file per study, golden per bump.
5. **Mesh + results are derived, never stored** (element-names/BOM rule);
   results stamped with `Document::Version()`, regeneration marks them
   **stale but never auto-solves** (solving is expensive and stays explicit
   — deliberately unlike A4's automatic assembly re-solve).
6. **Results rendering = a dedicated field renderer** (own
   pipeline/vertex format `{position, displacement, scalar}`, shader-side
   deform warp + analytic Turbo colormap); `Vertex3D` and the mesh pipeline
   untouched; shared UBO so the section plane clips stress plots for free.
7. **Headless-first phasing:** the RPC-driven mesh+solve loop (phases 0–2)
   ships before any panel or rendering exists — LLM parity from the first
   usable milestone, GUI as a consumer of the same core.

## Phasing

0. **FEM0 — spike (gate): DONE.** `shell.nix` + CMake `HAVE_NETGEN` (mesher
   built as its own `fem_mesher` lib so Netgen's PARALLEL/MPI/`-fabi-version=17`
   requirements stay contained); `FemMesher` wrapper + netgen-gated
   `test_fem.cpp`. Proved the C++ `OCCGeometry` path (the C nglib API can't
   attribute surface elements to faces), tet10 via explicit `MakeSecondOrder`,
   1:1 face attribution, per-face sizing, in-process reuse — no fallback needed.
1. **FEM1 — mesh: DONE.** Study model (`FemStudy`) + Commands + serialization
   (v29 + golden + studies/ dir) + `add_study`/`set_study`/refinement/load RPCs;
   `FemMesher` (global + per-face size, second order, stats, canonical tet10/
   tri6 ordering, error mapping) + `MeshStudy` orchestration + `mesh_study`;
   `test_fem.cpp` mesher + command cases.
2. **FEM2 — solve: DONE.** material E/ν/yield; `FemSolver` (tet10 assembly,
   Dirichlet elimination, `SimplicialLDLT`, consistent loads, von Mises
   recovery); `RunStudy` + Document result cache (derived, stale-not-autosolved);
   `run_study`/`study_results` RPCs with factor of safety; analytic benchmark
   suite (bar/cantilever/pressure). **The LLM loop is closed** — verified
   headless (set variable → run_study → read stress vs yield → iterate).
3. **FEM3 — display: DONE.** Simulation mode (enter/exit workspace like sketch
   mode — no FEM UI until entered): toolbar swap, Studies panel (list, loads via
   viewport face-picking, run, results + FoS + stale badge, field/deform
   controls), and the `FieldRenderer` (own pipeline/`FieldVertex`, Turbo
   colormap, shader deform warp, section-clip via shared UBO, hidden-body
   swap-in, peak marker, colorbar legend). `enter_simulation`/`exit_simulation`/
   `set_result_display` RPCs drive it headlessly. Verified via dev-watch.
4. **FEM4 — integration: DONE.** MCP mirrors for every study RPC;
   `tests/fem_smoke.py` (headless fix+load+solve+iterate, self-skips without
   Netgen); docs. VTU export left as a future nicety (the XCAF export path
   doesn't fit FEM field data; a raw `.vtu` writer can come later).

Order rationale: 0 de-risks the only external unknown for the price of a
day; 1 before 2 because the solver is meaningless without elements to
assemble; 2 before 3 per the headless-core rule (and it makes phase 3 purely
presentational); modal analysis (FEM5) and assembly studies get their own
design when wanted, on top of an already-proven solver core.
