# A study that spans more than one part (S1–S4)

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
own.** The placement a structural study needs already exists in the assembly
and is maintained there; a second home for it would be a second thing to
drift. This is the one point where the CAM analogy does not carry: a job's
layout is genuinely new information (parts laid flat on a sheet, three
fixture frames, one stock), while a structural study wants exactly the
assembly's own layout.

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

**S1 — studies on the assembly, one occurrence meshed, contacts clamped.**
The widest and least risky. `AssemblyDocument` gains a studies vector (it is
a separate class from `Document`, so it does not inherit `m_studies`); the 20
study RPC rows carrying `NeedsPart` accept an assembly tab; a `Load` gains a
contact anchor beside its face anchor; the fuzzy general fuse that finds a
shared patch lands in core, next to `Measure::ContactBetween`, which already
answers *whether* two placed parts touch but not *where* as a face. Format
bump plus an assembly golden. Delivers the ring-brace number.

**S2 — a neighbour meshed and bonded.** `StudyPlan` gains a mode where
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
the one most exposed to the eigensolver's limits.

Rough sizes, for sequencing rather than scheduling: S1 3–4 days, S2 2–4, S4
3–5. S3 is unestimated until the defeaturing rule is chosen. Per-part studies
are untouched throughout and remain the fast path, which matters given S3.

## Verification

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
