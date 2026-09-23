# Simulation (FEM)

Linear structural analysis of parts and assemblies: static stress and
displacement, and natural frequencies. Topology optimization is the third
kind of study and has its own page, [../topopt/README.md](../topopt/README.md).

## The model

A **study** (`fem::Study`, `src/core/fem/FemStudy.h`) is a document-level
object, not a `Feature`. It consumes the finished bodies and produces no
geometry, so it lives beside the parameter table (`Document::GetStudies()`,
`AssemblyDocument::GetStudies()`) and never enters regeneration, rollback or
suppression. Its analysis `type` is `static`, `modal` or `topology`.

A study holds:

- `meshSize` (mm; 0 = auto, the body's bounding-box diagonal / 15) and
  per-face `refinements` (a target element size on one face).
- `loads`: each a `Fixed` support, a `Force` (a total vector in newtons,
  spread as uniform traction over the face) or a `Pressure` (MPa along the
  inward normal, positive pushes into the part).
- `gravity` (static only; m/s² in world axes, 0 = off; needs a material
  density) and `numModes` (modal only; default 6).
- `participants` (assembly studies only, below).

Sizes and magnitudes follow the value + expression convention, so a load can
read `bolt_preload * 1.5`. Expressions are evaluated lazily at mesh/solve
time (`EvaluateStudyExprs`), not during regeneration.

Every face is an `ElementRef` resolved at mesh time (`fem::ResolveFace`,
the same lookup `ShellFeature` and `Measure` use). A reference whose face no
longer exists is a study error naming the load, never a silent skip. A face
may be given as `{feature_id, element}` or the element name alone; the body
that holds that name is then found, because the name's own prefix is the
feature that created the element, which after a boolean is not the body's
`created_by`.

A force with no direction pushes into the face it was put on
(`fem::DefaultForceDirection`, the negated outward normal). The GUI placement
and `add_load` both call it, so clicking and scripting agree.

### Units

The solver works in mm–N–MPa (lengths and displacements mm, forces N,
stresses and moduli MPa) with mass in tonnes, so frequencies come out in Hz.
The material library (`src/cad/CADMaterials.h`) stores `youngGPa`, `poisson`,
`yieldMPa` and `density` (kg/m³), each 0 when unknown. A body whose material
has no stiffness is refused ("has no stiffness data"); a modal study or a
gravity load on a material with no density is refused too. The library's
default material has none of these, so an unassigned body cannot be analysed
by accident.

### Results and staleness

Mesh and results are **derived** and are never written to a project
(topology results are the one exception; see the topopt page). They live in
a transient `fem::StudyRuntime` cache stamped with the document's `Version()`
at solve time. Any later change makes them **stale**: the panel badges them
and `study_results` / `list_studies` report `stale: true`. Nothing is ever
re-solved automatically — meshing and solving take seconds to minutes and
stay an explicit act. A study reopened from disk carries its setup and no
answers; `study_results` refuses it with "study has not been run".

## Meshing

`fem::Mesh` (`src/core/fem/FemMesher.{h,cpp}`) wraps Netgen's C++
`OCCGeometry` interface: it meshes a `TopoDS_Shape` directly, attributes
every surface element to its B-Rep face (the plain C nglib API cannot), and
produces second-order tetrahedra through an explicit `MakeSecondOrder()`, so
midside nodes lie on the true surface. Node order is canonical tet10 (corners
0–3, then edges (0,1)(1,2)(0,2)(0,3)(1,3)(2,3)); boundary triangles are tri6.
Mesh statistics carry node and element counts and the worst normalized tet
quality.

Netgen is optional. The wrapper is its own static library, `fem_mesher`,
because Netgen's build requirements (the `PARALLEL`/MPI defines and
`-fabi-version=17` the nixpkgs package was built with — a mismatch corrupts
the heap) must not leak into the rest of core. Without Netgen the app builds
and runs, and meshing returns "built without FEM support". Netgen writes
triangulation onto the faces it meshes, so `PlanStudyMesh` / `PlanStudy`
deep-copy each shape on the main thread and the worker meshes the copies;
the document's shape is never written under the renderer.

A mesh failure is reported as the study error, naming the face when Netgen
says which. There is no defeaturing step: suppress cosmetic features before
meshing.

## The solver

`src/core/fem/FemSolver.{h,cpp}` and `FemSolverInternal.h` (the element math
shared with topology optimization), namespace `fem`, GUI-free. Gated
`HAVE_FEM_SOLVER` on Eigen and `HAVE_SPECTRA` on Spectra (modal only).

- **Static** (`fem::Solve`): tet10 isotropic elasticity, 4-point Gauss rule,
  triplet assembly into a sparse K, fixed nodes eliminated (all three DOF),
  consistent nodal loads from face tractions (equal splitting is visibly
  wrong on tet10), `SimplicialLDLT`. The factorization can report success on
  a singular K, so the residual `‖Ku − F‖` is checked and an
  under-constrained model is refused as such. Stresses are evaluated at each
  element's nodes and averaged to the mesh nodes weighted by element volume;
  von Mises per node, and the maxima with their locations.
- **Modal** (`fem::SolveModal`): `K φ = ω² M φ` with a consistent mass
  matrix, lowest `numModes` by Spectra shift-invert. It needs a fixed support,
  so K is positive definite: only the lower triangle is assembled (the only
  half Spectra reads) and `(K − σM)⁻¹` is factorized with `SimplicialLDLT`
  behind `SpdShiftInvert` rather than Spectra's stock `SparseLU` path, which
  would store an L and a U for a symmetric matrix. Mode shapes are normalized
  to max |u| = 1.
- **Several materials in one mesh**: both solvers take one `Material` per
  mesh domain (`FemMesh::tetDomain`), which is what a bonded assembly group
  needs.

Per-part bodies are independent: a part study meshes and solves each body it
references separately, with no contact between them, and `study_results`
reports one entry per body. Only bodies that carry a load or refinement are
meshed; each needs a fixed support ("has loads but no fixed support" is
checked before any factorization).

`study_results` for a static study reports `max_von_mises_mpa` and
`max_displacement_mm` (`{value, at, body}`), the per-body breakdown, and a
factor of safety against the material's yield strength when one is known. The
factor travels with a receipt whose basis is *predicted*, and which is
unverifiable when the result is stale or no body has a yield strength.

### Threads

`RunStudy` is `PlanStudy` (main thread: expressions, face resolution,
validation, shape copies) + `MeshPlannedStudy` + `SolvePreparedStudy`
(plain data, safe on a worker). `run_study {async: true}` and the Run button
mesh and solve off the frame as a background job; `mesh_study {async: true}`
meshes only. A static or modal solve cannot be cancelled (the linear solver
has no interruption points). One study run at a time. A finished job stores
its result into the document it ran on even if another tab is active.

## Assembly studies

A study on an `AssemblyDocument` names **occurrences**, each in one role
(`fem::Participant`, top-level occurrences; a sub-assembly takes part whole):

| role | cost | meaning |
|---|---|---|
| `meshed` | mesh + solve | the part being analysed; exactly one per study |
| `bonded` | mesh + solve | meshed too, and glued to the meshed part where they touch, with its own material |
| `rigid` | one boolean | a support; its contact patch can be clamped |
| `ignored` | nothing | not in the analysis (also: any occurrence not listed) |

Left out and rigid bracket the truth: a neighbour left out is a lower bound
on stiffness, the same neighbour rigid is an upper bound, and bonded lies
between them. `study_results` carries the role table so a reader knows which
bound each neighbour holds.

Every anchor on an assembly study names its occurrence (`occurrence` on a
load or refinement). A fixed support may instead anchor on a **contact**
(`add_load {type: fixed, contact_with}`): the patch where a rigid participant
touches the meshed group. That patch exists in neither part, so it is
re-derived every time the study meshes, and a clamp on a rigid support that
touches nothing meshed is refused by name. Being rigid does not clamp
anything by itself; the clamp is a load.

How it is built (`src/core/fem/AssemblyStudy.cpp`, `fem::PlanAssemblyStudy`):

- **The imprint never touches a part.** Placed bodies are copied, and one
  general fuse (`BOPAlgo_Builder`) runs over the copies with
  `SetNonDestructive` — an OpenCASCADE boolean writes to its arguments. A
  split written into a part would reach its drawings, CAM and exports.
- **The fuse is fuzzy** (`SetFuzzyValue(1e-3)` mm). Parts are placed at
  float32, so faces that should coincide sit microns apart; a strict boolean
  finds nothing between them and the assembly meshes as loose pieces, which
  looks exactly like a working study.
- **A bonded group is one conforming mesh.** The fuse imprints the shared
  faces and keeps each solid, and Netgen meshes a shared face once, so the
  tets on both sides share its nodes and load crosses the joint without
  constraint equations. An unfused compound of the same solids meshes as
  separate components with every interface node doubled. The members'
  imprinted solids go into one compound whose faces are indexed the way the
  mesher indexes them (`TopExp::MapShapes`). Which mesh domain is which member
  is decided by classifying tet centroids against the members, not by
  trusting the mesher's numbering. The group reports as one body.
- **Bonding is declared, not inferred.** T-nuts, drivers and press fits are
  not glue, and fusing a line-to-line fit produces slivers that fail to mesh.
- **Refusals before meshing**: not exactly one meshed occurrence; a meshed,
  bonded or rigid pair that interpenetrates (checked with `interference`;
  `ignored` is the way out); a bonded member sharing no face with the group; a
  contact anchor whose support does not touch.
- Anchors on split faces are mapped onto the imprinted faces; a force's total
  is divided among them by area.

Assembly results are stale against the assembly's version **and** every
participating part's version (`StudyRuntime::solvedPartVersions`). An edit to
an assembly study is one full-state undo snapshot and one version bump. The
result cache (`fem::StudyRuntimeCache`) is shared between the document and
its undo snapshots so a snapshot does not copy meshes. Topology
optimization is refused on an assembly; `mesh_study {async}` is refused there
(`run_study {async}` works).

## Results display

`FieldRenderer` (`src/render/FieldRenderer.{h,cpp}`) has its own pipeline and
vertex format `FieldVertex {position, displacement, scalar}`: the boundary
triangles of the solved mesh, warped in the vertex shader by a push-constant
deformation scale (so the slider needs no re-upload) and coloured through an
analytic Turbo colormap. It shares the scene's clip planes, so a section
cuts a stress plot like everything else. While a field is shown it stands in
for the solved body (or every solved occurrence of an assembly study).
Fields: von Mises, displacement magnitude, a modal mode shape, and the
topology views. The legend names the study and analysis, the quantity with
quarter ticks, the deformation factor, the mode and frequency of a modal
result, and where the maximum is, marked in the viewport by a cross.

`set_result_display` colours the raster viewport, which is what `screenshot`
captures. `render_still` renders the model, not the field.

Boundary conditions are drawn in the viewport (`src/ui/StudyPanel.cpp`):
each BC face is tinted through the mesh renderer's face highlight in its
family's colour; a force is one resultant arrow at the face; a pressure is a
field of arrows over the face, each along its own triangle's inward normal,
on a grid spaced from the view-scaled glyph length; a fixed face gets anchor
hatches; gravity is one arrow from the centre of mass of the meshed set,
drawn in the see-through channel because that point is inside the solid.
Labels with value and unit sit at the glyphs and can be switched off. On an
assembly study a rigid support is drawn as a ghost with its contact faces
tinted as a fixture, and bonded seams in the bond colour.

## GUI

- A part enters simulation from the toolbar (command `part.simulation`); the
  Model dock lists the studies, one row per study with its state (solved,
  stale, failed, not run, solving) and Delete in the row menu. The selected
  study is edited, run and read in the floating **Simulation** window.
- On an assembly tab, `assembly.simulation` opens the Simulation tool window,
  a viewport mode beside explode and motion; it holds the study list and a
  participants table with a role per row and **Clamp here** on rigid rows.
- In the window: analysis type, mesh size, material check, gravity (a
  checkbox that sets world-down 9.81 m/s²), and **Boundary conditions**: arm
  Fixed, Force or Pressure and click a face; magnitude and an optional custom
  direction apply to the next placement. A placed force can be dragged in the
  viewport to change its magnitude (one undo step on release). Run shows the
  shared progress bar. Results: mesh size, max von Mises, max displacement,
  field selector and deformation slider; for modal, the natural frequencies
  (selecting one shows its mode shape) and a mode-shape toggle. The factor of
  safety is reported over RPC only.
- Clicking an occurrence other than the meshed one on an assembly study is
  refused in the panel.

## RPCs

All in `src/remote/RemoteSimulation.cpp` (`SimulationMethods` table). They
act on the active part or assembly; a drawing or job tab is refused, naming
where studies live. A verb that acts on a study takes `id` (or `study`); a
verb that names a study for context (`add_load`, `add_refinement`,
`set_result_display`) takes `study`.

| method | what it does |
|---|---|
| `add_study {type?, name?}` | `type`: static \| modal \| topology (`kind` accepted) |
| `remove_study`, `list_studies` | list reports type, mesh size, loads, `has_results`, `stale`, participants |
| `set_study` | `mesh_size`, `type`, `gravity` [x,y,z] m/s², `num_modes`, `participants`, topology parameters |
| `add_load {study, type, face \| contact_with, occurrence?, magnitude?, direction?}` | returns the load id |
| `remove_load {study, id}` | by load id |
| `add_refinement {study, face, size, occurrence?}`, `remove_refinement` | local element size, mm |
| `mesh_study {id, async?}` | per-body nodes, elements, worst quality |
| `run_study {id, async?}` | mesh + solve; returns the `study_results` summary when synchronous |
| `study_status`, `cancel_study` | progress of an async job; cancel applies to topology only |
| `study_results` | static: maxima, per-body, factor of safety; modal: `bodies[i].frequencies_hz` |
| `enter_simulation {study?}`, `exit_simulation` | the GUI mode |
| `set_result_display {study?, field, deform_scale?, mode?, threshold?}` | `field`: von_mises \| displacement \| shape \| density \| none |

## Tests

- `tests/test_fem.cpp` (`[fem]`): element-level checks (tet10 stiffness and
  stress without a mesher), mesher cases, and analytic benchmarks — uniaxial
  bar σ = F/A, cantilever deflection against Euler–Bernoulli, a Lamé
  thick-wall cylinder, modal f₁ against beam theory, self-weight deflection. It asserts physics with tolerances and
  never golden meshes, because Netgen output is not bit-stable across
  versions. Also: a mesh spans two solids only when they share the face; the
  assembly cases (clamped nodes on the interface only, the refusals by name,
  staleness across documents, the shared cache, a bonded pair with two
  materials and an answer softer than the rigid one).
- `tests/fem_smoke.py`, `tests/fem_async_smoke.py`: the RPC surface
  headless, including an assembly tab. Both skip (exit 77) on a build without
  Netgen, which is how the stock-Ubuntu CI jobs build; run them locally.
- GUI (`src/app/guitests/GuiTestsSimulation.cpp`, category `sim`): force
  direction and arrow placement, pressure as a field, face tints, gravity at
  the centre of mass, a static run against beam theory, the assembly study
  window and its clamp, the list-row editor.
- `tests/rpc_gate_smoke.py` checks the study verbs are accepted on an
  assembly tab.

## Not done

- Contact that can separate or slide, and nonlinear analysis (plasticity,
  large deformation), thermal, buckling, fatigue. The escape hatch if they
  are ever wanted is an external solver such as CalculiX, not growing this
  one past linear.
- Free-free modal analysis (a modal study needs a support).
- Shell and beam elements; adaptive refinement.
- Defeaturing for analysis. Mesh size is set by the smallest features, so a
  whole cabinet of 16 mm panels with fastener holes meshes to millions of
  DOF; per-panel studies are the practical route until holes can be
  suppressed for the analysis shape.
- Per-participant mesh size on a bonded member applies to its faces only; the
  study's global size still governs the interior.
- Result export (VTU).
