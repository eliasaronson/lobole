# Assembly solver

How mates place occurrences: the connector frames a mate joins, the mate types
and their maths, the closed-form placement pass, and the numeric pass that
closes mate loops. Code: `src/core/AssemblySolve.{h,cpp}` (`Solve`,
`ConnectorLocalFrame`), `src/core/AssemblyLoopSolve.{h,cpp}` (`SolveLoops`).
The model is in [README.md](README.md).

## Mates are joints between connector frames

A mate connects two **connectors** — a frame on each of two occurrences — with
one mate type. The type is the *normal form* of a constraint stack: a hinge is
one revolute mate, not "concentric + coincident" whose hinge-ness is implicit.
The DOF two rigid bodies can be left with are the lower kinematic pairs, and
those are the types.

### Connector frames

A connector names an occurrence and an **anchor**; the frame (origin, X, Y, Z
in the part's coordinates) is derived from the anchor at solve time and never
stored, so a part edit moves the connector with the geometry.

| anchor | frame |
|---|---|
| `origin: true` | the part's coordinate frame |
| `frame: <id>` | a named Frame feature of the part — the mate as designed (a bolt-circle centre, a shaft axis) — or a DatumPlane (origin, X, normal as Z) or DatumAxis (origin, direction as Z, X the world's X laid square to it), which is how a scan is mated ([datums on a scan](../modeling/README.md#datums-on-a-scan)). An errored one refuses the mate. |
| `face: {feature_id, element}` planar | origin at the face centroid, Z the outward normal, X a deterministic in-plane axis (`CADShape::FacePlane`) |
| … cylindrical | Z the axis, origin the face centroid projected onto the axis, X the cylinder's **seam** (`CADShape::FaceAxis`; see below) |
| … conical | Z the axis, origin the apex, X the cone's seam |
| … spherical | origin the centre, Z world +Z |
| `snap_edge` (with a face) | origin moved to that edge's characteristic point — a circular edge's centre, else its midpoint; the face still gives Z. An edge that no longer resolves errors the connector rather than leaving the origin at the face's centroid, a different point |
| `snap_edge` + `snap_edge2` | origin at the vertex the two edges share (a corner). A corner has no persistent name of its own, but two named edges do; when they share none or more than one vertex, the origin falls back to `snap_edge`'s own point rather than guess. |

Faces and edges are toponaming `ElementRef`s
([../modeling/README.md](../modeling/README.md)). A face that no
longer resolves puts the **mate** in an error state naming which connector
failed and, when the stored name is a prefix of current ones (a later feature
modified the face), suggesting them. An unnamed face cannot be picked for a
mate. Connectors on a placed sub-assembly use `inner_path` (the occurrence ids
down to the part); the frame comes back in the top-level occurrence's space.

**A round face's X is its seam.** A cylinder or cone has no in-plane axis of
its own to offer, so X is the surface's reference direction — where its
parameter u is 0, which is where the seam of a whole cylinder lies. It is
the surface's, not the seam edge's: it comes from the feature that made the
surface (a sketched circle's is its sketch plane's U axis, an imported
cylinder's its placement), so it is the same after every
regeneration and survives a boolean that splits or trims the face, which
keeps the surface even where it moves the seam edge. So a frame on a worm,
a cam or a keyed shaft is reproducible from the face alone, and a mate's
`rotation` counts from a direction the part fixes, not from the world (until
v150 it was a world-axis heuristic, the same for every Z-axis cylinder).
Frame features built on a round face take the same X, and their own rotation
turns the frame from there.

Centring falls out of the connector choice: a cylindrical-face connector sits
on the axis, so two cylinders mate coaxially; a hole-centre snap does the same
for a bolt in a bore. A planar-face connector sits at the centroid, so two
planar faces coincide centroid to centroid — mate a cylinder or a hole centre,
plus an axial `offset`, to centre a part.

### Mate types

Every mate stores the same fields; the type decides which are fixed
parameters and which are the live DOF. Because the DOF value is stored, a file
re-solves to identical placements, and a drag or `set_mate` just writes the
field.

| type | free | DOF fields | fixed parameters |
|---|---|---|---|
| fastened | nothing (0) | — | `rotation` (authored twist), `offset`, `dx`, `dy` |
| revolute | turn about Z (1) | `rotation` | `offset` (axial seat along the shaft) |
| slider | slide along Z (1) | `offset` | `rotation` |
| cylindrical | turn + slide (2) | `rotation`, `offset` | — |
| planar | slide in plane + spin (3) | `dx`, `dy`, `rotation` | `offset` (plane gap) |
| ball | any rotation (3) | `ball_rotation` (quaternion) | — |

`flipped` **defaults to true**: b's Z is reversed against a's, which is what
two touching faces want (opposed normals). Two seat *frames* usually want
`flipped: false`. `rotation` is radians in the file and in answers; an edit
takes `rotation_deg` only ([units](../conventions.md#units-and-angles)).

### The alignment

With `Wa` a's connector frame in the world and `Fb` b's connector frame in b's
part:

```
Adjust = T(dx, dy, offset) · Rz(rotation) · R_ball · Flip
T_b    = Wa · Adjust · Fb⁻¹
```

`Flip` is 180° about X. The translation is applied in **a's connector frame**,
so a positive `offset` moves b along a's +Z (a's outward normal). A reference
face whose normal points into the assembly therefore reads as the "wrong"
world sign; the convention is asserted by `test_assembly_solve.cpp`.

`MateAdjustFor` (`AssemblyLoopSolve.cpp`) is the one definition of this
composition; the placement pass and the loop solver both call it, in double
like every placement ([../conventions.md](../conventions.md#the-model-is-double)).

`DeriveMateDofs` inverts it: `add_mate {derive: "twist"}` reads `flipped` and
`rotation` off where the two occurrences already lie (anchors coincident),
`"pose"` reads all five so the mate holds the parts exactly where they are. It
refuses when the two frames are not square to one axis — a nearest quarter
turn there would be a guess. `fit_fastener` builds its mates the same way.

## The placement pass

`assembly::Solve`:

1. **Relations propagate** first ([relations.md](relations.md)); in a mate
   tree that is their enforcement.
2. **Frames.** Both connector frames of every mate are resolved. A mate that
   cannot resolve is errored and sits out; a mate suppressed by the active
   configuration (or touching a suppressed occurrence) sits out without an
   error. DOFs the configuration *holds* overlay the mates' own values for the
   whole solve and are restored after.
3. **Anchors.** Every `fixed`, unsuppressed occurrence is placed at its stored
   transform.
4. **Propagate.** Mates are scanned in list order; a mate with exactly one side
   placed places the other by the alignment above. Repeat to a fixpoint. If a
   mate still has an unplaced side, the **lowest-id** unplaced occurrence among
   usable mates is anchored at its stored transform (a floating component
   stays where it was left) and propagation resumes.
5. **Cut mates.** A mate that finds both sides already placed closes a loop
   and goes into the cut set. With no cut mates nothing numeric runs, so a
   loop-free assembly is placed purely in closed form.
6. **Close loops** (below) if there are cut mates.
7. **Write back** solved transforms to non-fixed occurrences; unmated
   occurrences keep theirs. Then `ApplyAttachments` re-seats riders.
8. **Check relations**: every equality relation must now hold (see
   [relations.md](relations.md#when-relations-disagree)).

`Solve` does not bump the assembly version; the caller does, so a no-op solve
does not churn the renderer. A solve that knows what was just driven (a drag,
`set_mate`, a motion sample) passes it as `Driven`, and that DOF is held while
loops close.

Mate errors are transient (like a feature's error): cleared and re-stamped on
every solve, never saved. Placements always stand.

## Closing loops

A loop is closed in **reduced (joint) coordinates**: the unknowns are the
mates' own DOF fields, which the document already stores. The forward map from
those values to world placements is the tree walk above. So there is no new
state and no format involvement — the solver needs only a residual, a
Jacobian and a factorization.

- **Components.** The mate graph is split into connected components and each
  component with a cut mate is solved on its own. A relation joins its
  mates' components into one: two fixed occurrences no mate joins are two
  anchors, and a cam turning on one with its follower's loop on the other
  would otherwise be two problems, the relation a row in neither, and the
  loop's solve would move the follower back from where the relation set it. Otherwise one small loop
  would put a column for every mate in the document into the Jacobian (each
  column a full tree walk), and independent loops stay independent. Placements
  from the loop pass are **merged** into the walk's result, never substituted
  for it, so mate trees elsewhere keep their placements.
- **Unknowns** are every free DOF of every mate in the component, tree mates
  and cut mates alike. The component's anchor is pinned, which fixes the
  gauge.
- **Residual.** For each cut mate, the 6-vector between where the mate says b's
  frame should be and where the tree put it: translation in mm, then the
  rotation (log of the rotation part) scaled by a characteristic length — the
  spread of the component's occurrence origins at the seed, at least 1 mm — so
  every row is in mm and the tolerance means one thing. Relation rows (one
  per relation, however many mates it couples) and pinned-DOF rows (the driven DOF, configuration holds) are added beside them.
- **Precision.** Evaluation runs in double, as the placements and mate values
  it starts from and writes back are. A float chain of matrices carried
  ~1e-3 mm of noise at 100 mm scale, which a finite difference at small h
  turns into a useless Jacobian.
- **Jacobian:** forward differences, one tree walk per column
  (h = 1e-7 · max(1, |q|)). It cannot disagree with the forward map.
- **Solve:** Levenberg–Marquardt from the stored values, at most 100
  iterations, converging when the worst row is under 1e-5 mm. A column that
  moves the residual by no more than the 1e-3 mm tolerance per whole unit of
  its DOF is **frozen** at its seed: Marquardt's damping is relative to the
  diagonal, so an uninfluential DOF would otherwise be handed whatever
  satisfies 0 = 0. This matters because every bolt in a pattern closes a small
  loop, pulling everything hanging off that component (a lid's hinge, say)
  into the loop problem.
- **A component with no unknowns** (a bolt pattern: all fastened) skips LM and
  the factorization: one walk, one residual per cut mate.

### Classification

After the solve, `rank` comes from a column-pivoted QR of Jᵀ (so the pivoting
runs over the rows of J):

| residual | rows | verdict |
|---|---|---|
| ≤ 1e-3 mm | — | closed; `dof = columns − rank` |
| ≤ 1e-3 mm | all six rows of a cut mate dependent | that mate is **redundant but consistent** — named in `redundant_mates`, status `redundant`, not an error (the extra bolts through a flange) |
| > 1e-3 mm | — | the cut mate errors: `loop does not close (off by N mm)`, N being that mate's own residual, followed by the diagnosis below when it names a suspect |

A mate is redundant only when **all six** of its rows are dependent: a planar
four-bar's closing joint has three structurally-zero rows (a loop of parallel
axes says nothing about z or tilt) while being the joint that makes it a
linkage.

DOF comes from the rank, not from a Grübler/Kutzbach count: a planar
four-bar in 3D scores 6·3 − 5·4 = −2 ("over-constrained") by Grübler, while
its rank gives 4 − 3 = 1. `dof` covers the looped components only.

### Naming the mate that keeps a loop open

The mate that carries a loop's error is the cut mate, and which mate is the
cut depends on anchors and creation order, not on which mate is wrong. The
common real fault is a revolute, cylindrical or slider connector whose axis is
tilted — the joint values cannot express a tilt, so the whole mismatch lands
on the cut, which may be three joints away. So when a component fails to
close, `DiagnoseLoop` (`src/core/AssemblyLoopSolve.cpp`) asks which single
connector, if its axis were allowed to tilt, would let the loop close:

- **Trials.** For each connector of each axis-carrying mate in the failing
  component — tree or cut, side A and side B — the component is re-solved
  with two more unknowns: a rotation of that connector's frame about its own
  X and Y, at its origin. A's tilt is composed before the joint motion (the
  joint turns about, or slides along, the tilted axis); B's after it (the
  part riding the joint is tilted). Both sides are tried because a wrong
  frame on B cannot be written as one on A: for a revolute only at one joint
  angle, for a slider not at all. Everything else — relations, pins, seed,
  tolerance — is the normal solve.
- **Closing is not enough.** Two extra unknowns give a planar loop as many
  unknowns as equations, so a tilt at nearly any joint downstream of the
  fault closes it too — as a rigid spatial linkage, at one isolated pose. A
  tilt that undoes the fault gives back the mechanism's mobility (a planar
  four-bar's one DOF). So each trial records, beside its residual and tilt,
  the rank DOF of the joint columns with its tilt held (a pivot counts below
  1e-4 of the largest, since a restored plane is restored only to the
  closure tolerance).
- **Suspects** are the trials that close (residual ≤ 1e-3 mm) with the
  highest DOF among those that close, one entry per mate; if both of a
  mate's connectors qualify, it is named without a side.

What the message says follows the numbers:

| suspects | the cut mates' error gains |
|---|---|
| one | `; mate 'crank pin' connector A (crank) closes it if its axis tilts 30.0° - check that connector` |
| several | `; any one of mate 'a' connector B (…) (30.0°) or mate 'b' connector A (…) (30.0°) closes it with that tilt; the loop cannot say which - check their connectors` |
| none | nothing: the fault is not a single tilted axis (a pin shifted along its axis, a wrong length) |

Several suspects is a real tie, not a failure to decide: every joint of a
loop that meets at one point absorbs a rotation equally, and a tilt about a
link's own line is the same whichever of that link's two connectors carries
it. Neither is ever resolved by picking one.

It runs after the solve, on the failed loop, and not when a mate is created:
before the first solve the angle between two connectors is wherever the parts
were dropped, so a check at `add_mate` would fire on nearly every correct
mate. Removing mates one at a time tells nothing either — removing any mate
of a single loop leaves a tree, which always closes.

A closed loop runs nothing extra. A failing one runs two extra solves per
axis-carrying mate of the failing component, each with two more columns. A
motion study's per-sample solve (`assembly::Solve(…, diagnose = false)`, from
`MotionStudy.cpp` and the motion panel's playback) skips it: a sample that
fails is reported with its residual, and nothing reads the text.

### Loop mates are outputs

Inside a loop a mate's DOF is an output — turn the crank and the rocker
follows — so the loop pass writes the solved DOFs back to the mates. The
file then reopens where it converged, and the next solve seeds from there. A
loop-free document is never dirtied by a solve.

### Branches

A mechanism can close on more than one branch (a four-bar's open and crossed
modes); which one a solve lands on is a property of the seed. Seeding from
the stored values keeps it on the branch it is on, and motion studies walk
their samples in order for the same reason (`test_assembly_loops.cpp`, "A
four-bar seeded on a branch stays on it"). A step too large for its seed
can still land on another branch with every loop closed, so a motion study
watches the joints' rates and re-solves such a step finer
([branch jumps](README.md#branch-jumps)); the solver itself has no branch
guard.

### Coupled loops

A Theo Jansen leg is the coupled-loop case: eight bodies, ten revolutes,
three independent loops in one component, and one degree of freedom by
rank. Its three knees are dyads, each with a mirror branch that closes
every loop just as exactly, so a jump to one cannot be seen in the
residual. Only the knee's signed angle shows it: which side of the line
through its two other joints the vertex is on. `test_assembly_loops.cpp`,
"A Jansen leg has one degree of freedom and walks its own curve", drives the
crank a full turn at 2 degrees a step, seeded from the last step as a motion
study is. It asserts closure, all three signs at every step, and the foot
against the leg's own circle-intersection kinematics.

Seeded from the previous step the solver stays on the built branch at
every step up to 30 degrees. At 45 degrees a step it lands on a mirror
branch, first at 360 degrees, and stays there, still closed. A motion study
no longer follows it there: it sees the joints' rates jump, re-solves those
steps finer, and comes back on the built branch
([branch jumps](README.md#branch-jumps)); with `max_refine: 0` it reports
the jump instead. The Strandbeest still checks its knees' signs at every
sample of its 2-degree study, as a second witness
([its README](../../generators/strandbeest/README.md#checked-every-run)).

The twelve-leg walker is the largest component yet: 109 revolute columns
(twelve legs of nine joints and the crank) and 36 cut mates in one solve.
Seeded from the last sample, it costs about 36 ms a sample on the
development machine (6.6 s for a 181-sample turn with no clash check). That
is fast enough that analytic Jacobians were not built ([cost](#cost)).

### Without Eigen

The loop pass needs Eigen (`HAVE_EIGEN`). A build without it places trees as
usual and errors every cut mate with "this build has no Eigen, so mate loops
are reported, not solved" rather than placing something plausible.

## Reports

`solve_assembly` returns every mate with a `status` (`satisfied`,
`redundant`, `error`) and its error text, `errored_mates`, `placed`,
`occurrences`,
`placed_by_mates` / `placed_by_transform`, and — only when there are loops —
`loop_mates`, `dof`, `loops_closed`, `residual_mm` (the worst),
`redundant_mates`. A mate whose loop did not close carries its own
`residual_mm`, in `solve_assembly`'s `mates[]` and in `list_mates` (the value
from the last solve). When a loop did not close, `suspects[]` lists the
diagnosis's suspects as `{mate, name, tilt_deg, connector?, closes, sole}`:
`closes` is the ids of that loop's failing mates, `sole` is false when the
loop has several equally good suspects, and `connector` (`a`/`b`) is absent
when either side does. `add_mate` needs none of this to be useful: its
`errored[]` carries each errored mate's text, diagnosis included, so the call
that breaks a loop says which connector to look at.

## Cost

The floor is one tree walk. A bolt pattern costs about the same as no loop
(zero unknowns). A genuine mechanism costs one tree walk per unknown per LM
iteration; analytic Jacobians (a revolute's column is a cross product) are the
lever if that ever matters. It has not yet: the Strandbeest's 109 columns
solve in about 36 ms a sample ([coupled loops](#coupled-loops)). A loop that fails to close also pays for its
diagnosis: two more solves per axis-carrying mate in that component.
