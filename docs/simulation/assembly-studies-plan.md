# A study that spans more than one part (S0–S4)

Extends [plan.md](plan.md), whose "Assembly studies / contact" deferral this
picks up. The technique it rests on is answered — see the P0 field note there
and `tests/test_fem.cpp`, "a mesh spans two solids only when they SHARE the
face".

## What we have

`fem::Study` lives in `Document::m_studies`, an assembly tab refuses a study
("the active document is an assembly, not a part"), and within one part each
body is meshed and solved SEPARATELY — one eigenproblem per body, no shared
stiffness matrix. So a study can only ever be about one panel, and nothing
can express "these panels, glued to each other".

The cost is not abstract. The smart speaker's side wall measures 311 Hz
(`generators/smart_speaker/modal.py`), and that number is a LOWER BOUND that
ignores every brace which actually touches the panel, because a support must
be a face and a butt joint lands on a continuous one. The ring brace and the
ladder both cross that wall; treating them as supports puts its regions near
515 and 593 Hz. The only brace the study could use is the divider, which
happens to sit in a 4 mm dado — and is the one brace far enough from the
antinode to change nothing. **The measurement's silence is an artefact of
boundary conditions, not a finding about the design.**

## What is already right

Almost all of it, for the same reason the CAM job move was "a rehoming, not a
rewrite" ([../cam/job-document.md](../cam/job-document.md)):

- **The mesher and the solver are document-independent.** `fem::Mesh(const
  TopoDS_Shape&, …)` and `Solve(const FemMesh&, …)` / `SolveModal(…)` know
  nothing about documents. Shape in, mesh in, results out.
- **`StudyBodyPlan` already owns a copy of the shape** at plan time
  (`src/core/fem/FemSolver.h`, `TopoDS_Shape shape; // a copy, owned here`).
  That is where an imprint belongs, and why this is not surgery.
- **The assembly already holds the placements**, and the mates keep them
  correct as parameters move. `FlattenActiveAssembly` hands back placed leaf
  parts with `world`, `partId`, `rootOccurrenceId` and `innerIds`.
- **A conforming mesh across a glued interface works** (P0): a general fuse
  imprints the shared face, keeps both solids so they can carry different
  materials, and Netgen meshes that face once. Tied constraint equations are
  not needed.

## The shape of it

**Studies live on the `AssemblyDocument`, not in a document kind of their
own.** The CAM job became a kind because a job holds information the part
does not — parts laid on a sheet, a fixture frame each, one stock — and it
cost eight and a half sessions ([../cam/job-document.md](../cam/job-document.md):
77 `DocKind` sites, every `cam_*` verb taking an explicit `job`, the golden
corpus moved). A study holds no layout: it is a question asked of a model,
the way a per-part study is a question asked of a part and a tolerance is a
statement about one. The rule that puts a part's studies on the part puts an
assembly's on the assembly. A `Study` kind would either split the two homes
(part studies on `Document`, assembly studies in their own file, two panels
and two gates) or rehome the twenty study RPCs, the panel, the serialization
and every generator that drives them, for no capability the assembly home
lacks. The assembly tab already floats its viewport modes as tool windows
(explode, motion — [../ui/u15-assembly-panel.md](../ui/u15-assembly-panel.md)),
so simulation is a third window there, not a new tab.

What DOES carry over from the job is its anchor. A job target is
`{jobPartId, ElementRef}`; an assembly study's is `{occurrence, ElementRef}`,
the occurrence keyed by its flattened path (`rootOccurrenceId` + `innerIds`,
the key `interference` and `Flatten` already use), so a sub-assembly's leaf
names the same thing everywhere. Every anchor in an assembly study — a load's
face, a fixture's face, a refinement's face — is occurrence-qualified; the
contact anchor below is one more kind of anchor, not the only one an assembly
study has. The cabinet's feet are fixed by their faces, on their occurrences.

**Which assembly owns a study made in context.** A part does not know its
assemblies, and a panel can sit in several. So a study that names a
neighbour is the ASSEMBLY's study, whichever tab it was authored on: the
in-context part tab shows the assemblies' studies whose meshed occurrence is
the part being edited, and creates into the assembly that is ghosted around
it. Nothing about a neighbour is ever written into a part document.

**A study names OCCURRENCES, each in one of three roles:**

| role | what it costs | what it means |
|---|---|---|
| **meshed** | mesh + solve | a participant; its own flexibility is in the answer |
| **rigid support** | one boolean | its contact patches are clamped on the parts it touches |
| **ignored** | nothing | not in the analysis at all |

Absent and rigid BRACKET the truth — a part left out is a lower bound on
stiffness, the same part rigid is an upper bound, and meshed is the number
between them. A study that reports the pair is more honest than one that
reports a single figure, because the clamped-versus-pinned idealisation is
worth a factor of two on a panel and dwarfs most things being compared.

**The imprint never touches the part.** It is computed into the study's
meshing shape and discarded, exactly as `BodiesInSetupFrame` transforms
bodies into setup space without editing the document. A split written into a
part's history would turn up later in its CAM job, its drawings and its
exported STEP: a simulation would have edited the model.

**A fixture anchors on the CONTACT, not on a face.** "The contact with «port
ring brace»", re-derived every time the study meshes — because the patch does
not exist in the part, and because an anchor that is re-derived survives a
rebuild for the same reason a mate anchored on a frame does and an element
string does not.

**Bonding is declared, not inferred.** P0 fused all 40 placed solids of the
cabinet: the boolean passed and the mesh then failed on the port tube's
line-to-line d105 fit into its flare, which fusing turns into slivers.
T-nuts, drivers and a press fit are not glue. What is bonded to what is an
input.

**`SetFuzzyValue` is part of the contract, not a tuning knob.** Parts are
placed at float32, so nominally coincident faces sit microns apart. A strict
boolean finds nothing between them and the assembly meshes as loose pieces —
which looks exactly like a working study. 1e-3 imprints across a 3 µm gap.

**Stale means stale against every document the study read.** Today
`StudyResultsStale` compares `solvedAtVersion` to the owning document's
`Version()`. An assembly study reads the assembly (placements, mates, the
active configuration, the joint pose of a mechanism) AND each participating
part's geometry, so its results record the assembly version plus one version
per participating part, the way `DrawingSourceVersion` records a view's
source, and go stale when any of them moves. A panel re-generated behind a
fresh-looking result is the same lie as a stale drawing.

**A study refuses what it cannot mean, before it meshes.** Two occurrences
that are meshed or bonded together and INTERPENETRATE (a press fit, a
fastener modelled inside its hole) fuse into slivers, which is what P0 hit on
the port tube; the study runs `interference` on its participant pairs first
and refuses naming the pair, with `ignored` as the named way out. A fused
shape that is not a valid B-Rep already fails at the boolean (52ec32ae). A
mechanism with free joints is solved at its current pose and the results say
which pose; a configuration switch is a version move like any other.

**Mesh size is per participant.** A 16 mm panel and a 60 mm brace do not
want one global size; each role row carries its own size (value + expr, the
study's global size as the default), beside the per-face refinements a part
study already has.

### Through the GUI

The gesture already exists: `edit_in_context` activates an occurrence's part
tab with the assembly ghosted around it. So the neighbours are on screen
before anything is asked of them, and a support is picked the way a load's
face is picked today — by clicking the ghost, not by adding rows to a list of
parts. What lands in the study's list is "rigid — contact with «port ring
brace»", with one toggle per row to make it meshed instead.

Two surfaces, because there are two questions:

- **a panel in context** (part tab, assembly ghosted): how does *this* panel
  behave, given what holds it. Neighbours are supports.
- **the assembly** (assembly tab): how does the whole box behave. Occurrences
  are participants, and only what you fix is fixed — for a cabinet, its feet.

## Phases

**S0 — a setup you can read, and a result that says what it is.** SHIPPED
2026-09-16 for part studies ([../ui/u11-simulation-setup.md](../ui/u11-simulation-setup.md),
"S0"); the role ghosts and patch tint wait for S1. Part
scope, so it lands first and S1 inherits it; nothing in S1–S4 depends on it,
and every part study benefits today. What is drawn now
(`src/ui/StudyPanel.cpp`, `AppendSimulationBCGlyphs`): one glyph per load at
the face CENTRE — a red anchor hatch for fixed, one orange arrow for a force,
one short yellow arrow for a pressure — no tint on the face it acts on,
gravity not drawn at all, and the results legend a Turbo bar with a maximum
and a zero. A pressure over a whole baffle reads as a point load at its
middle, and "what is actually fixed" is a question the picture cannot answer
once two faces are adjacent. The shape every FEM tool has settled on:

- **The face is the boundary condition; the glyph is its type.** Every BC
  face is tinted through the mesh renderer's face-highlight channel
  (`MeshRenderer::SetFaceHighlight` — additive per instance, so it already
  handles several faces and several occurrences) in a colour per family:
  fixtures one colour, loads another, contact patches (S1) the fixture
  colour. Needs a colour on the highlight call, which today knows only the
  selection tint. Colours come from the theme so light and dark both read.
- **A pressure is a field, not an arrow.** Sampled over the face's triangles
  on a grid whose spacing is DERIVED from the glyph length (the defaults
  rule: count from face area, never a literal), every arrow along the local
  surface normal, pointing INTO the face. A force keeps one resultant arrow
  at the face centroid on top of the tinted face, which is what "total force
  spread as traction" looks like. Fixed keeps the anchor hatch, repeated on
  the same sparse grid rather than once at the centre.
- **Gravity is drawn.** One arrow at the centre of mass of the meshed set,
  world-down, labelled with its acceleration; a self-weight study whose
  picture shows no weight is a setup nobody can check.
- **Every BC carries a label.** Value and unit at the glyph's anchor —
  "1.2 kN", "0.5 MPa", "fixed", later "contact: port ring brace" — in the
  viewport draw list (`ui::ViewportDrawList`, behind windows since
  9ea3c70c), switchable off in the panel.
- **A legend that states the claim.** Study name and analysis type, the
  quantity with its unit, minimum and maximum with their locations, the
  deformation scale as a factor ("×120") next to the picture it distorts,
  and for a modal result the mode number and frequency. Today the peak is a
  magenta cross the legend does not mention.
- **Ghosts say their role (S1).** A rigid support is drawn as the in-context
  ghost with its contact PATCH tinted — the patch the imprint computed, not
  the whole face it lives on, so the picture shows exactly what is clamped.
  Ignored occurrences are drawn faint or hidden; meshed ones as bodies. A
  bonded interface (S2) is a tinted seam in a third colour.

Sizes stay view-scaled the way the glyph length already is. The same code
draws a part study and an assembly study — the assembly study simply has
more anchors.

**S1 — studies on the assembly, one occurrence meshed, contacts clamped.**
SHIPPED 2026-09-16 (format v106); see "S1 as built" below.
The widest and least risky. `AssemblyDocument` gains a studies vector (it is
a separate class from `Document`, so it does not inherit `m_studies`) and the
multi-document stale record; the 20 study RPC rows carrying `NeedsPart` take
a flag that admits an assembly tab (the dispatch gate in
`RemoteDispatch.cpp` refuses them there today, and `rpc_gate_smoke` is what
catches a row left behind); anchors become occurrence-qualified and a `Load`
gains the contact anchor; the role table with its per-row mesh size; the
interference pre-flight; the fuzzy general fuse that finds a shared patch
lands in core, next to `Measure::ContactBetween`, which already answers
*whether* two placed parts touch but not *where* as a face; the patch tint
and role ghosts from S0. `study_results` carries the role table so a reader
knows which bound each neighbour holds. Format bump plus an assembly golden.
Delivers the ring-brace number.

**S2 — a neighbour meshed and bonded.** SHIPPED 2026-09-17 (format v107);
see "S2 as built" below. `StudyPlan` gains a mode where
several solids become ONE `FemMesh` rather than one per body, reusing S1's
imprint to make the interface shared. `Solve` takes a single `Material`
today, so this is also where materials become per-element — the stiffness
assembly loop, simple in concept and hot in practice. Needed the moment the
plywood coverings meet MDF.

**S3 — defeaturing for analysis.** A prerequisite, not a nicety. The
cabinet's 18 structural solids at a coarse 40 mm size come to 524,405 tets
and 837,279 nodes — about 2.5M DOF, 73 s to mesh. A uniform 40 mm fill of
that volume would be ~1,100 tets: what drives the count is 16 mm panels
capping the local size and several hundred fastener holes refining it
further. Suppressing those holes for the analysis shape is what makes S4
worth attempting, and it belongs in the same shape pipeline as the imprint.

**S4 — the whole box.** S2's meshing with N occurrences instead of two, plus
reporting and result display at that scale. Technically the smallest step and
the one most exposed to the eigensolver's limits — and to memory: a
`StudyRuntime` at 2.5M DOF is held whole today, so S4 states a budget and
decimates what the field renderer is handed rather than what is solved.

Rough sizes, for sequencing rather than scheduling: S0 2–3 days, S1 3–4, S2
2–4, S4 3–5. S3 is unestimated until the defeaturing rule is chosen. Per-part studies
are untouched throughout and remain the fast path, which matters given S3.

## S1 as built

What landed on 2026-09-16, and where it differs from the paragraphs above.

- **The record.** `fem::Study` gained `participants` (occurrence, role
  meshed|rigid|ignored, its own mesh size), loads and refinements gained
  `occurrenceId`, and a fixed load gained `contactWith` — a contact anchor
  with no face. `AssemblyDocument` holds its studies with the part's
  accessors, and a result cache SHARED with its undo snapshots (a full-state
  copy must not duplicate a mesh). Stale is measured against the assembly's
  version and every participant part's version.
- **The plan.** `fem::PlanAssemblyStudy` (src/core/fem/AssemblyStudy.cpp):
  one meshed occurrence or a refusal naming both; the interference
  pre-flight, refusing by name; a fuzzy `BOPAlgo_Builder` general fuse on
  COPIES, `SetNonDestructive`, so no document's shape is written; the
  imprinted solid found by centroid (the builder's `Modified` answers for
  faces and edges, not reliably for solids); anchors mapped onto the
  imprinted faces, a force's total split by area; the contact patch as the
  faces both imprinted solids share, plus the rigid body's ORIGINAL faces
  that carry it, for the picture. The result meshes and solves through the
  unchanged `MeshPlannedStudy` / `SolvePreparedStudy`.
- **One handler, two homes.** The twenty study RPCs lost `NeedsPart` and
  resolve the tab themselves (`Host` in RemoteSimulation.cpp): `set_study
  {participants: [{occurrence, role, mesh_size?}]}`, `add_load
  {occurrence, face}` or `{contact_with}`, `add_refinement {occurrence}`;
  `list_studies` and `study_results` carry the role table. An edit on an
  assembly is one full-state snapshot and one version bump. The GUI reads
  the same host (`ui::StudyHost`), so the panel body, the glyphs, the
  tints, the labels, the legend and the field overlay are one set of code:
  on the assembly tab they live in the **Simulation tool window**, the
  third floating viewport mode beside explode and motion, with a
  participants table and a "Clamp here" on each rigid row.
- **The picture.** A rigid support is a ghost with its contact faces tinted
  as the fixture they are; a click on any occurrence but the meshed one is
  refused in the panel, not dropped. The contact patches are planned on
  demand and cached against the study's text and the documents' versions.
- **Narrower than planned, on purpose.** A participant is a TOP-LEVEL
  occurrence; a sub-assembly participates whole (its leaves placed by
  `Flatten`), which is what a bracket bolted to a unit wants, and the path
  key above is not needed until someone wants one leaf of a unit alone. The
  contact tint is the rigid body's whole touching face, not the patch cut
  out of it — the seam is S2's. An assembly study meshes synchronously; the
  solve runs off the frame (`run_study async: true`, the Run button).
- **The ring-brace number.** On the cabinet (`generators/smart_speaker/modal.py
  --assembly`), the side wall meshed at 25 mm with its neighbours as rigid
  supports clamped where they touch:

  | supports | f1 | f2..f6 |
  |---|---|---|
  | the four walls and the divider, as contacts | **352.5 Hz** | 408, 510, 662, 860, 924 |
  | the same plus the brace ladder and the port ring brace | **562.4 Hz** | 1096, 1100, 1160, 1452, 1517 |

  The part study clamped at the wall's EDGES gave 311 Hz and said of itself
  that clamping the edges stretched the span from 320 to 352 mm, so it read
  about 20% low: the contacts land 16 mm inboard and give 352. And the
  braces — the measurement the part study could not make, because a brace
  that butts against a continuous face has no face to clamp — lift the
  fundamental to 562 Hz, inside the 515 to 593 Hz the hand calculation gives
  for the floor-to-brace regions. Twelve seconds a solve.
- **Verified by** `tests/test_fem.cpp` (S1: fixed nodes on the interface
  plane only; the six refusals by name; stale across documents; the shared
  cache), `tests/fem_smoke.py` (the whole verb set on an assembly tab, stale
  on an assembly edit AND on a part edit, both pre-flight refusals), and
  `sim/an_assembly_study_clamps_where_the_support_touches` (the window, the
  clamp, one undo, the refusal, the ghost and the tint, the run).

## S2 as built

- **A role, not a flag.** `bonded` beside meshed, rigid and ignored: meshed
  AND glued to the meshed occurrence where the two touch. Declared per
  occurrence, so "what is bonded to what" is an input, as the plan asked.
- **One fuse, one compound, one mesh.** `PlanBondedGroup`
  (src/core/fem/AssemblyStudy.cpp): every member solid and every rigid body
  through one fuzzy non-destructive general fuse; the members' imprinted
  solids in one compound, whose faces are indexed the way the mesher indexes
  them (`TopExp::MapShapes`, a shared face once); anchors on any member's
  face; contacts with the rigid supports as in S1; a bonded member that
  shares no face with the group is refused by name, as is any member pair
  that shares volume.
- **A material per domain.** `FemMesh` records the domain of every tet
  (netgen's solid index) and `Solve` / `SolveModal` take one material per
  domain — stiffness, mass and stress recovery each with their own
  elasticity. Which domain is which member is answered by geometry (a tet
  centroid classified against the members), not by trusting the mesher's
  numbering.
- **The picture.** The glued seams are tinted in the bond colour on both
  sides (the whole faces that carry them), and the field overlay replaces
  every solved occurrence's meshes at once. The group reports as one body,
  "Upper + Lower".
- **Verified by** `tests/test_fem.cpp` (S2: one mesh component, no doubled
  interface nodes, two domains, steel above z = 16 and aluminium below, the
  fixed nodes on the lower block's bottom only, and the bonded answer
  SOFTER than the rigid support's — the bracket the plan asked for) and
  `tests/fem_smoke.py` (the bonded study over the RPCs, one body for the
  group, softer than rigid).
- **Not yet.** Per-member mesh size is honoured on a bonded member's faces
  but the study's global size still governs the interior; the seam tint is
  the whole face, not the patch.

## Verification

**CI cannot run any of the meshing cases.** The ubuntu jobs build without
Netgen ("Netgen not found: FEM meshing disabled" in every run), so
`fem_smoke` and `fem_async_smoke` are SKIPPED there and P0's own case does
not run. Every assertion below is verified locally
(`ctest -L fem` under nix) until CI gets a Netgen job; the plan does not
claim otherwise.

- **S0**: a GUI case per BC type that reads the glyph vertices back and
  asserts they lie ON the face (a pull's arrow was once drawn wholly inside
  the solid and depth-tested away); a pressure over a large face producing
  more than one arrow and a small one at least one; the gravity arrow at the
  centre of mass; a screenshot of the speaker's baffle study, looked at.
  The `defaults/*` suite covers the derived grid spacing.
- **S1**: a C++ case in `tests/test_fem.cpp` — two placed boxes touching on a
  face, one meshed with the other as a rigid support, asserting the clamped
  nodes lie on the patch and nowhere else; and the refusal when the named
  occurrence does not touch the studied one. A python smoke in
  `tests/fem_smoke.py` for the wire form on an assembly tab. Acceptance:
  `generators/smart_speaker/modal.py` gains the side wall with the ring brace
  as a support and lands near the 515 Hz the hand calculation gives for its
  floor-to-brace region.
- **S2**: the same two boxes, bonded, must fall BETWEEN the same pair absent
  and rigid. That bracket is the assertion — a bonded answer outside it is
  wrong whatever the mesh says.
- **S3**: element count on the cabinet's structural subset, before and after,
  with the modes of one panel unchanged beyond a stated tolerance — a
  defeature that moves the answer is not a defeature.
- **S4**: one connected component over the whole structural subset (P0's
  measure), and a modal solve that completes.

## Not in this round

**Contact** — parts that can separate or slide. A glued wooden box does not
need it, and it is the branch that brings nonlinear iteration. Bonded is the
physically right model here, and P0 showed it needs no new solver.
