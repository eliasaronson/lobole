# A blend beside faces an `add` left in two pieces

## The problem

An `add` fuses two solids and keeps the faces they share a plane (or a
cylinder) on as separate faces, with an edge between them, until a Refine.
OpenCASCADE's blend builder fails where a blend's end or its run meets that
edge, and the size search reads the builder's failure at every size as the
size's: an L of two boxes refuses its wall's round at every size ("no radius
builds … down to 0.00257 mm") where the same L from one profile rounds R2.5
to its closed form; a cylinder flush with a bar refuses its rim chamfer past
"0.121 mm", where 7.5 builds once the bottom is one face. The width bound
(`WidthAcrossEdgeAt`) measures the face to the edge between the pieces, so
the size it names is wrong as well as the build
([known bug](../known-bugs.md#a-blend-beside-a-plane-an-add-left-in-two-pieces-is-refused-at-every-size)).

## Where the faces are merged

Two places can merge them; the choice is what it costs in element names.

**(a) In every `add`** (a `topo::RefineResult` on the fuse, as a modelled
thread does). Every add that leaves two same-domain faces renames them:
the pieces become one face named after the first piece with `;M<add>`,
the other piece's name dies, and so does every reference to it saved by a
document -- a sketch on the back of a two-box L, a mate on a flush boss's
side -- whether or not anything is ever blended there. The faces a user
sees change for every add.

**(b) Inside the blend, beside its edges only, and only where it fails.**
The fillet or chamfer builds on the body as it is, as now; where the
builder fails there (or builds an invalid solid) it builds again on a copy
whose same-domain faces are merged where the blend meets the edge between
them, and names that result from the input through the merge and the blend
composed (`BRepTools_History::Merge`, as `BooleanNamed` composes a
clean-up). Nothing changes for a blend that builds today: no add, no
document, no name. Where the merged build is kept, the merged face takes
the first piece's name with `;M<blend>` -- what that piece would be called
after the blend -- and the other piece's name ends at the blend, as a face a
blend consumes does. Merging always (whether or not the body as it is
builds) also moves the names of blends that build today: a recorded
session that rounds a 180-degree revolve's two coplanar caps and then
places a feature on the second cap no longer replays
(`tests/fuzz/cases/boolean_failed-779.jsonl`).

(b) is built. Which faces it merges:

- an edge between two faces is a **split** when the faces lie on one
  surface (`ShapeUpgrade_UnifySameDomain` decides; a cheap pre-test on the
  surfaces -- coplanar planes, coaxial cylinders, cones, spheres, tori of
  one size -- keeps the unifier off bodies that have none);
- it is merged when it shares a vertex with an edge of the blend (a picked
  edge, or one the builder's tangent propagation brings in,
  `topo::EdgeBlendContours`), or, between two planes, when one of them is a
  face the blend touches: the end of a round on an L's two-piece end face
  reaches the edge between the pieces at its setback, away from the
  round's own vertex. Curved splits on a touched face are not merged: two
  halves of an earlier round's torus merged under a later round's chain
  build an invalid solid;
- everything else is kept (`KeepShapes`: every other edge, and every vertex
  but a merged split's ends), so the unifier merges nothing else and
  joins no other edges;
- the size that fits measures on the merged body, and a candidate builds
  when either the body as it is or the merged copy does.

The size that fits measures on the merged body too: the bounds
(`fit::BlendBounds`, `fit::RefusingBounds`) and the search's candidates run
on it, so a width is measured to the face's real boundary, not to the edge
between its pieces. The bound sentences name faces by the input's names
(a merged face by its first piece).

## Files

- `src/core/ElementHistory.{h,cpp}`: `topo::MergeBesideBlend` (the merge,
  its history, the picked edges' indices on the merged body);
  `EdgeBlendNamed` and `EdgeBlendUnnamed` build on it, `RunEdgeBlend`
  names through the composed history; `topo::BlendBody` gives the merged
  body with the input's names for the measures.
- `src/core/features/FilletFeature.cpp`: bounds and search on that body.
- `tests/test_blend_fit.cpp`, `tests/test_toponaming*.cpp`: the L of two
  boxes and the rim beside a flush bar, built through the feature: the
  closed-form volume, the refusal's size that fits builds and 2 % above it
  does not, and the names (a reference to the merged piece follows; one to
  the other piece fails, naming the blend).
- docs: modeling/README.md (the size that fits; a new subsection), 
  toponaming.md (fillet/chamfer naming), known-bugs.md (entry removed),
  kernel case rows updated.

## What (a) and (b) cost, measured

Against main, on the goldens (`[golden]`), the kernel case corpus and the
action matrix (241 designs):

- (a) moves 89 element names the matrix keys on in three designs (the
  hinge box's box and lid, fuzz/8), breaks a translated case's `add`, and
  passes 5 of the 13 rows (`solid.combine` is a Boolean, not an add, so the
  851 cases stay refused).
- (b) merging always moves no name the matrix keys on but fuzz/37's (its
  own fillet meets a split: one face of a positional family fewer), refuses
  a hinge box's knuckle chamfers the body as it is builds, and breaks the
  recorded revolve session above; merging only where the body as it is
  fails moves none of these, and passes 9 of the rows (the tenth builds as
  it is, a patch more) and lets 14755's first chamfer build. The goldens
  pass under every variant.

## Verification

Measured under (a) and (b) against main: the goldens (`[golden]`), the
kernel case corpus (which rows move), and the action matrix (every changed
name or answer). (b) must move no golden and no action-matrix name except
where a blend touches a split, and must pass the 12 rows the finder's
Refine-after-every-add experiment passed. The headless, GUI and pygui
suites after.
