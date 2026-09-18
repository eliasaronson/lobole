# 3-axis surfacing — finishing a surface the levels cannot reach

## The problem

Every toolpath this app generates is a contour at a level. `DepthPasses` walks
Z in steps, `SectionLoops` cuts the part at each one, Clipper2 offsets the
result. That is the whole pipeline, and it is why a curved or sloped surface
comes out as a staircase: the levels are the only Z the cutter ever visits
inside a pass.

So three ordinary things cannot be finished today, and the SECOND is the one
that makes this worth doing — it is not exotic geometry:

- a curved face — `PickedFaceRegion` refuses it, "pick a flat face — that one
  is curved" (`CamPicks.cpp:159`);
- a **tilted planar** face — refused as "a level face — a tilted one has no
  single depth" (`:166`), and nothing else machines it either: it is not a
  wall (`IsWallFace` wants `|normal.z| < 0.01`) and `EdgeRound` only follows a
  wall's top edge. Draft faces, angled mounting pads and wide chamfers are all
  this case, on otherwise prismatic parts;
- the floor left by a ball nose, whose scallops only the verifier ever sees.

Roughing is NOT part of this. A curved part is roughed at levels — `Adaptive`
and `Pocket` already do that correctly — and then finished. Every comparable
tool agrees: roughing is an operation the user adds, never one generated for
them. This plan adds the finishing half and changes nothing about the other.

## Phase 0 — before the enum grows

Two more `OpType` values make several existing weaknesses worse, because the
cost of each is linear in the number of op types. Three of them are live
defects today, independent of surfacing. This phase is worth doing on its own
merits; it is listed first because doing it afterwards means retro-fitting six
types plus the two new ones.

### A missing table row must not become a profile

`OpTypeName()` returns `"profile"` for an enum value with no row
(`CamNames.cpp:23-28`) and `FollowsOneTargetAtATime()` returns `false`
(`:46-52`). Nothing catches the gap: the only census test iterates `OpTypes()`,
so it checks the rows that exist and cannot see a missing one, and
`test_golden.cpp:85-90` is a ternary chain that also ends `: "profile"`.

So a surfacing op whose row is forgotten or mis-ordered **saves as
`"type": "profile"` and reloads as a full-depth contour round the part**, with
the golden test passing while it does. That is the silent substitution
`CamNames.h:44-48` says the file exists to abolish — the name→type direction
was hardened and type→name was not.

`OpType::Count`, a `static_assert` that `kRows` covers it, and both fallbacks
made unreachable so a caller must refuse rather than receive a plausible wrong
answer. The `test_golden.cpp` ternary goes through the same table.

### A missing switch case must not compile

`DeriveOperation`'s switch (`CamDerive.cpp:177-415`) has no `default:`, so an
unhandled type falls through and returns `ok = true` with a **default**
`Operation` — top 0, bottom -1, no target. That bug already shipped once for
`Adaptive` and `tests/test_cam.cpp:1870-1875` is named after it. `-Werror` is
deliberately off tree-wide (`CMakeLists.txt:91`), so today the compiler only
warns into a build log.

`-Werror=switch` for `src/core/cam/` alone turns that failure mode into a
build break, which is the whole value of having written the dispatch as an
exhaustive switch in the first place.

### Serialization writes by value, not by type

`SerializeCam.cpp:441` writes `stepover` only for `Pocket | Face | EdgeRound`.
`Adaptive` reads that field (`CamAdaptive.cpp:81`, `engine.stepOverFactor`),
the panel offers the slider for it (`CamPanel.cpp:1384-1386`) and the RPC
accepts it (`RemoteCam.cpp:457`) — but there is one write site and one load
site, and the loader defaults to 0.45 (`:603`).

**So an adaptive op set to 0.25 engagement reloads at 0.45.** The one number
adaptive clearing exists to hold constant does not survive a save.
`tests/golden/job/v73-cam-clearing-side.json:67` carries `"stepover": 0.25` on
an adaptive op and the loss is invisible, because the golden check compares op
type NAMES and not numeric fields.

Every op field is written when it differs from its default, never when its
type is on a list. And the golden check grows numeric op-field comparison, so
the round-trip gate can fail — a gate that cannot fail is why this survived.

**No `kVersion` bump.** No key is added, removed, renamed or reinterpreted:
the loader already read `stepover` unconditionally, and a file written before
or after loads to the same document. What changes is that the writer stops
dropping a value it was asked to keep. A version bump says readers must handle
something new, and here none must.

### `stepover` is not a length, and the units gate is green on saying it is

Three `MethodDoc` rows state *"Every length here is MILLIMETRES — top_z,
bottom_z, stepdown, stepover, peck…"* (`RemoteCam.cpp:176-177`, and again at
`:223-224` and `:245-246`). It is a fraction of tool diameter, clamped
0.05-0.9 (`CamAdaptive.cpp:81`, panel bounds `CamPanel.cpp:1390-1391`).
`rpc_units_gate.py:51` classifies `stepover` under the length stems, so the
gate is **satisfied by prose stating the wrong unit** — the one failure the
gate exists to prevent, passing inside CAM today.

The rows say what it is: a fraction of the cutter's diameter, dimensionless.
The gate accepts that wording, and accepts it honestly. This matters more the
moment `sampleStep_mm` and `zStep_mm` — genuinely millimetres — appear in the
same rows beside it.

### One table answers "does this op have this field"

`cam::Operation` (`CamJob.h:184-277`) is a union with no discriminant. Six
fields are already dead for four or five of the six existing types; the panel
shows tabs on a drill with a tooltip admitting "(profile only)"; and
`ApplyOpFields` (`RemoteCam.cpp:396-470`) accepts every field for every type
with no refusal at all.

`stepover` alone has four disagreeing answers — the panel shows it for four
types, the serializer writes it for three, `OpJson` reports it for **one**
(`RemoteCam.cpp:538-541`, so a pocket's stepover is settable and unreadable),
and `CamDerive` sets it for all six.

`OpTypeRow` already is the capability table; it carries one flag. It grows the
columns the scattered predicates are re-deriving locally — cuts at depth,
follows a contour, takes tabs, has a stepdown, reads a stepover — and the
serializer, the panel, `OpJson`, `ApplyOpFields`, the `CamFeeds` checks and the
refusals all read the row. The agent-facing type list in the RPC prose
(`RemoteCam.cpp:178-179`) comes from `OpTypeNameList()` rather than being a
second hand-maintained copy.

This is what makes surfacing's ten refusals (below) one table column instead of
a tenth predicate chain.

### Files and verification

`core/cam/CamNames.{h,cpp}` · `core/cam/CamJob.h` · `CMakeLists.txt` ·
`core/io/SerializeCam.cpp` · `remote/RemoteCam.cpp` · `ui/CamPanel.cpp` ·
`ui/CamTool.cpp` · `core/cam/CamFeeds.cpp` · `tests/test_golden.cpp` ·
`tests/test_cam.cpp` · `SerializationInternal.h` + `docs/versioning/format.md`
+ one golden.

Verified by: a round-trip test that sets a non-default `stepover` on an
adaptive op and reads it back — it fails today, which is the point; the
`static_assert`; a build with `-Werror=switch` and a deliberately unhandled
type; `rpc_units_gate.py` still green, now truthfully; and the full suite,
since capability columns touch every op type.

## Phase 1 — surfacing

### What a surfacing pass is

A finishing pass is a **pattern** swept over a **contact surface**. Those are
independent, and keeping them independent is what makes an operation cheap:
pycam's whole surfacing generator is 91 lines because the pattern is a motion
grid it is handed, and blendercam's parallel strategy is 36 for the same
reason. The pattern is 2D; the contact algorithm puts it on the part.

The contact surface is the **cutter-location surface**: for each (x, y), the
lowest Z at which the tool can sit without cutting into the model. That is
`SampleModel` — which already builds the designed surface on a grid — dilated
by the tool's own profile. `TipHeight(tool, r)` is exactly the profile
function that dilation needs, and it already exists and is already trusted:
`CarveSweep` verifies ball, V and drill shapes with it.

```
z_CL(x,y) = max over cells p within the tool's radius of
              ( z_surface(p) - TipHeight(tool, |p - (x,y)|) )
```

**This is deliberately not opencamlib.** `TipHeight` is the same abstraction
as its `MillingCutter::height(r)`, but there that function serves only the
vertex-drop case; facet drop wants a three-scalar offset-surface descriptor
and edge drop a closed form per cutter, the toroidal one iterating with Brent
and warning past 200 iterations. A grid dilation buys vertex and facet
accuracy without any of it, and loses precision only on sharp convex edges —
which a finishing pass is not for. Against that, vendoring costs 6 043 lines
for a compilable drop-cutter subset (a further 3 083 for waterline, which
shares nothing with it but the cutter class), in C++14 with OpenMP and raw
owning pointers. If exact cutter-location points ever matter more than the
dependency, the door stays open: the pattern and the linking do not care which
contact algorithm produced the Z.

### Three quantities, three names

The single most common defect in the tools surveyed is one word meaning
several things. FreeCAD has three ops whose `StepOver` is, respectively, a
percentage of diameter, a percentage of diameter that is **not** the waterline
Z step and is hidden in two of three modes, and a distance per revolution —
and its rotational scan converts the first into degrees against the
circumference *at the current layer*, so it silently means something different
per layer. Kiri:Moto's one "step over" label is a fraction for an end mill and
an absolute distance for a taper mill, with bounds that vary from 0..1 to
0..100 across ops that share the tooltip.

So this plan names quantities, never "stepover":

- **`stepover`** — spacing ACROSS the passes, a fraction of tool diameter.
  Unchanged in meaning from the field we already have, and `DerivedStepover`
  already computes it correctly for a ball nose from the cusp equation.
- **`sampleStep_mm`** — spacing ALONG a pass, where the CL surface is
  evaluated. Every tool surveyed carries this as a separate quantity and none
  merges it into the stepover. It is derived from the grid, not typed.
- **`zStep_mm`** — the Z spacing between waterline levels. Waterline's
  meaningful control, and an XY fraction cannot express it.

`sampleStep_mm` derives from the CL grid's resolution and `zStep_mm` from the
tool and the requested scallop. Neither is a literal.

### What surfacing refuses

A surfacing operation has **no stepdown**. It is not a stack of levels, and a
depth-per-pass on it is meaningless. pycam states this most clearly by simply
not declaring the key for its surfacing strategy, so the widget never renders
and the grid is built with no layer distance. Ours must refuse it rather than
ignore it, per "refuse, don't substitute".

The same applies to every field on `Operation` that presupposes a contour and
discrete levels: `stepdown`, `side`, `tabCount`/`tabWidth`/`tabHeight`,
`cornerRelief`, `leadArcs`, `drilledBores`, `useOutline`, `peck`,
`finishTool`/`finishStock`, and `climb` (a bidirectional raster is half climb
by definition). Each is refused by name on a surfacing op, not silently
dropped — FreeCAD's Waterline persists eleven properties it unconditionally
hides, and its own `AngularDeflection` is a visible user property annotated
"is unused".

The lesson worth taking beyond this file: those ten fields are on `Operation`
because there is nowhere else to put them. FreeCAD does not have this problem
because tabs, dogbones, boundaries and lead-ins are separate wrapper objects an
op is decorated with, so an op that cannot take tabs is simply never wrapped.
That restructuring is not this plan, but it is the reason this plan has a list.

### The region, and the verb

A surfacing op needs a boundary to stay inside. `PickedFaceRegion` and
`PickedWallContour` cannot supply one: both refuse anything not axis-aligned.

`PickedSurfaceRegion` takes a face of any orientation, tessellates it,
projects its triangles to XY and unions them into loops — the face's
**silhouette**. Plus a no-pick form giving the whole part's silhouette, which
mirrors how `Face` and "cut out the whole part" already need no pick.

Being a B-Rep app is a real advantage here and should be spent. Kiri:Moto
needs a dihedral-angle flood fill to turn a clicked triangle into a face, with
a user-tunable threshold and the misgrouping that implies; a B-Rep face IS
that answer, exactly, with no threshold to get wrong. The pick we already have
resolves to a face; nothing more is needed.

Two verbs, not one with a flag:

- **`SurfaceParallel`** — raster at `cutAngleDeg`, good on shallow surfaces,
  poor on steep ones.
- **`SurfaceWaterline`** — constant-Z laps, the reverse.

They are separate because they answer opposite questions and because merging
them forces the stepover conflation above — a single op cannot honestly carry
both an XY fraction and a Z step under one name. FreeCAD demonstrates both
halves of this: its Surface op carries a `ScanType` flag that drags six dead
rotary properties into every planar job, and when the same workbench later
gained continuous rotary it was written as a **separate op** rather than a
fourth `ScanType`.

The verb should say what it does. A picked face contributes its outline, not
its depths — the Z always comes from the CL surface of the whole model, or a
pass would gouge the material standing next to the face it was given. "Finish
within this face" is the honest reading.

### Leaving room for the rotary axis

Continuous 4-axis stays deferred, with plan.md's reasons. What this plan owes
it is a shape, not an implementation — the same bargain C9 struck, where
committing to frame-per-setup and toolpaths-in-setup-space meant the indexed
axis needed no new toolpath maths.

The measurement worth designing against: FreeCAD's 3-axis surfacing cost 7 787
lines, and continuous rotary written afterwards cost 2 160 — **28%** — because
its generators are a dependency-free numeric core taking plain arrays, which
the rotary op could call directly. So:

- **The drop cutter takes a frame; it does not hard-code world -Z as the tool
  axis.** Rotary drop-cutting is drop-cutting in a rotated frame. This is one
  parameter, and it is the difference between reuse and a rewrite.
- **The pattern generators take and return plain arrays**, with no dependency
  on `Setup`, `Document` or OCC.
- **Name the quantities** (above), so rotary's axial pitch is a new field
  rather than a third meaning of `stepover`.

And explicitly NOT: no cylindrical stock mode, no generalised "surface
parameterisation" in the verifier, and no decision about what feed means on a
rotary move. Build the planar height map; the cylindrical one is a sibling.

One warning is worth recording now, because it is the thing every surveyed
tool got wrong. **Decide the rotary feed model before the rotary
representation, not after.** Neither Kiri:Moto nor blendercam compensates F
for an A move — there is no inverse-time mode anywhere in either — so surface
feed is wrong by the radius ratio; Kiri interpolates arc length only so the
*preview* looks right. Both made it an afterthought because their
representation was already "a point with an `a` field", which is exactly what
`Move::hasA` is. The one design that handles it makes the F-word's unit an
explicit user-visible mode, clamps the r->0 centreline singularity with a
stated maximum, and reports clamping once per operation rather than per move.

### Files

| file | what changes | lines |
|---|---|---|
| `core/cam/CamDropCutter.{h,cpp}` | new — CL surface by dilation, grid derivation, region mask, parallel over cells | 250-350 |
| `core/cam/CamSurface.cpp` | new — `GenerateParallel`, `GenerateWaterline` (marching squares on the CL surface) | 450-600 |
| `core/cam/CamToolpathInternal.h`, `CamStrategies.cpp` | `EmitPolyline3D` for a varying-Z pass; dispatch | 130 |
| `core/cam/CamJob.h` | two `OpType`s, `cutAngleDeg`, `sampleStep_mm`, `zStep_mm`, boundary mode | 90 |
| `core/cam/CamNames.cpp` | two rows | 4 |
| `core/cam/CamDerive.cpp` | derive both; refuse a flat mill on a slope, a V-bit; the field refusals | 150 |
| `core/cam/CamPicks.cpp` | `PickedSurfaceRegion` | 150 |
| `core/cam/CamPost.cpp` | **nothing** — `FitArc`'s `usable()` already requires a cut move at constant Z with no A word (`CamPost.cpp:334-337`), so a sloped raster fits no arcs and a constant-Z waterline lap correctly does. `LastCollinear` is fully 3D at 0.005 mm, well under any scallop, so the decimator will not flatten a pass either | 0 |
| `core/cam/CamTravel.cpp` | R6 finish-path guard | 10 |
| `core/io/SerializeCam.cpp` | new fields | 25 |
| `remote/RemoteCam.cpp` | `MethodDoc` rows, unit suffixes for `rpc_units_gate.py` | 40 |
| `ui/CamPanel.cpp`, `ui/CamTool.cpp` | per-op fields, the verb | 120 |
| `CamVerify.cpp`, `CamSim.cpp`, `CamStockMesh.cpp` | **nothing** — `CarveSweep`/`TipHeight` already carve ball and V | 0 |
| `tests/test_cam_surface.cpp` | new, `[slow]` | 400 |
| `tests/test_cam.cpp`, `test_cam_verify.cpp` | generate -> post -> verify a dome | 270 |
| `app/guitests/GuiTestsCam.cpp` | verb and panel | 80 |
| `SerializationInternal.h`, `docs/versioning/format.md` | `kVersion` bump + history row | 15 |
| `tests/golden/v<N>-surfacing.json` + `.expect.json` | required at the bump | 60 |

~2 200 lines. Consistent with what the surveyed tools spend on the parts that
correspond: pycam's drop cutter is 91 lines on a 321-line intersection core
with cutters at 170-340 each; blendercam's parallel is 36 on a shared sampler;
Kiri's surfacing ops are 47-169 line shims over one 617-line area engine. The
engine is the cost, not the ops.

### Verification

- The CL surface against shapes with known answers: a ball nose on a sphere
  sits at a computable offset; a flat mill on a plane sits on it; a ball in a
  concave corner cannot reach the corner, and must not claim to.
- **Closed loop**: generate, post, and run the existing verifier on a dome —
  `maxGouge` within tolerance and `leftoverArea` accounted for. This is the
  check that matters, and it works today only because `CarveSweep` models the
  real tool profile.
- Stepover against scallop: halving `stepover` must halve the cusp the
  verifier measures, not merely change the path count.
- The refusals: every field listed above, refused by name on a surfacing op.
- `render_pixel_smoke.py` byte-identical — surfacing meshes finely, and the
  drop cutter must mesh DETACHED or it re-tessellates the document.
