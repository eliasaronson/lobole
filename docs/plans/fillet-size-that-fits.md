# The size that fits

A Fillet, Chamfer or Shell that cannot be built at the size asked should say
the size that can: "the largest radius that fits is 7.5 mm", not "radius too
large for the geometry?".

## The problem

A blend that fails says so three ways, none with a number:

- `FilletFeature::Execute` (src/core/features/FilletFeature.cpp): "fillet
  failed (radius too large for the geometry?)", and for a result that
  crosses itself "a smaller radius, or fewer edges in one fillet, usually
  builds";
- `RunEdgeBlend` (src/core/ElementHistory.cpp): "the blend could not be
  built (radius too large for the geometry?)";
- `topo::EdgeBlendNamed`: "the fillet failed on 2 contour(s) (radius too
  large for the geometry?)" and where each contour is
  (`FaultyContourPlaces`).

It says where and never how much. The caller, a person dragging the field
or an agent sending `add_feature` again, bisects by hand, one build and one
round trip a guess. It is the commonest refusal a modeller gives: the
random designs count it as a `limit`, the action matrix as a refusal, and
neither can tell an honest limit from a blend the kernel should have built
just under it.

A Shell is half way there. `WallObjection`
(src/core/features/ShellFeature.cpp) measures the body under each open face
and says "A wall under 4 mm (2 mm, say) fits". A wall that passes it and
that the offset builder then refuses (an inner round smaller than the wall,
a rib thinner than two walls) gets the builder's sentence and no size.

The number has nowhere to go but the sentence. `add_feature` and
`modify_feature` refuse a feature that does not build ("Fillet refused: ...
The document is unchanged", src/remote/RemoteModeling.cpp); a feature an
upstream edit broke is a `regen.failed[]` row of `{index, name, error}`
(src/remote/RemoteHelpers.h). The tool window shows the error in red with
Accept disabled (src/ui/PreviewFeature.cpp).

## The shape of the fix

### 1. Measures at the picked edges

New `src/core/BlendFit.{h,cpp}`, reading the body the way `EdgeObjection`
already does. For each picked edge and each of its two faces:

- the **dihedral angle** between the faces, at the edge's middle;
- the face's **width across the edge**: how far the face runs from the
  edge, in the face, before its boundary (a planar face; the least over the
  edge's length), and whether the edge it ends at is picked too;
- the **least radius of curvature** of a face that curves toward the blend
  (the inside of a bore, a concave round);
- the edge's **own radius**, when it is a circle.

`Measure` gains the three as functions, so `query_geometry` can answer them
too (kernel-case-corpus.md uses them to describe a minimised failure).

### 2. Bounds: what cannot fit, for the cost of a look

A round of radius r set on an edge where the faces meet at θ takes
r / tan(θ/2) of each face; a chamfer takes its distance. From the measures:

- that setback is at most the face's width, and where the face's far edge
  is picked too the two setbacks together are (two rounds on a 15 mm end
  face meet at 7.5);
- r is at most a concave face's least radius of curvature;
- r is at most the radius of a convex circular rim (a cylinder's end
  rounded by its own radius is a hemisphere).

Each is an upper bound: nothing above it fits, and something below it may
still fail. The least of them, with the face or edge that gives it, is the
blend's bound.

### 3. A search of a fixed number of builds

When the build at the asked size fails, `fit::LargestThatBuilds` looks for
one that does. A candidate "builds" when `topo::EdgeBlendNamed` returns a
solid and `Measure::InvalidReasons` is empty, the two things `Execute`
requires. Every candidate is rounded down to three significant digits, so
the number reported is a number that was built.

- The first candidate is the bound, or the asked size where there is none
  below it. Where the bound is the true limit this is the only build.
- Then a bisection between zero and the least size known to fail, eight
  steps: a fixed count, so the answer does not depend on how busy the
  machine is.
- A blend has no deadline to share: `BRepFilletAPI_MakeFillet::Build`
  ignores its progress range in OpenCASCADE 7.9, and the one unbounded
  loop it had is capped by a patch
  (nix/patches/occt-fillet-orientation-probe.patch). So the search is
  bounded by its count, nine builds at most, and by the job it runs in:
  on a part the app holds it is cancellable with the regeneration, and a
  cancelled search reports what it established, the largest candidate that
  built and the smallest that failed.

A feature that stays broken is regenerated again and again. The search's
answer is kept with the feature's executed state (`CopyDerivedState`),
keyed on the input body, the edges and the operation, and reused for any
asked size at or above the size known to fail: a field dragged further past
the limit searches once.

### 4. What it says

- Past a bound, with no build: "fillet radius 10 mm does not fit: face
  'F' is 15 mm wide between the two picked edges, so the rounds meet at
  7.5 mm". A geometric fact, naming the element.
- The build failed and the search converged: "fillet radius 10 mm does not
  fit: the largest radius that fits is 7.49 mm", then the contour places
  as now.
- It was cancelled: "a radius of 5 mm builds and 7.5 mm does not (the
  search was stopped)".
- Nothing built down to a thousandth of the asked size: "no radius builds
  on these edges", with today's advice about fewer edges, which is then the
  true one.

A Chamfer says "distance". "radius too large for the geometry?" leaves the
source.

### 5. Where it is reported

`Feature` holds the answer beside its error, as a `FitHint`, set and
cleared with it.

- **The feature's error** is the sentence above: the tree row's tooltip,
  `get_state`, `check_geometry`'s `feature_errors`.
- **`regen.failed[]`** gains `fits` on that row: `{param: "radius",
  asked_mm, fits_mm, fails_mm?, basis: "bound" | "built", element?}`. A
  `set_variable` that grows a radius past its limit answers the size in its
  own response.
- **The refusal** of `add_feature` and `modify_feature` carries a
  `diagnostic` with `code: does_not_fit`, `param`, `observed` (the size
  asked) and the same `fits` object. The SDK's error
  (python/lobole/errors.py) exposes it.
- **The tool window** shows the sentence, and under it a button, "Use
  7.49 mm" (`###tw_fit`), which types that value into the field as the
  user would: the preview builds and Accept comes back. The committed
  feature's editor has the same button. A field bound to an expression has
  no button; the sentence is enough, and the variable is the user's to
  change.

### 6. Bounds that refuse before the build

`FilletFeature::WhyRefusedOn` asks the bounds, so a size past one is
refused in milliseconds by `add_feature`, `modify_feature` and Accept
alike, with the first sentence of section 4 and `basis: "bound"`. A bound
kind refuses there only once the kernel-case corpus's limit ladders and an
action-matrix sweep show OpenCASCADE builds nothing past it; until then it
only seeds the search. The matrix's ratchet holds this: a pass that
becomes a refusal fails the run.

### 7. Shell

A wall `WallObjection` passes and the offset builder refuses is searched
the same way on `thickness`, inside the Shell's one deadline, the first
candidate half the body's thinnest wall (`ThinnestWall`). An offset costs
seconds where a blend costs milliseconds, so the search is four steps, and
on a body whose first draw used most of the budget it reports the bracket
it has. The message ends "the largest wall that fits is 0.7 mm" beside the
builder's reason.

### 8. `fit: "reduce"`, on the call

`add_feature` and `modify_feature` take `fit`, beside `feature`: `"exact"`
(the default, everything above) or `"reduce"`. Any other value is refused
naming the two; `fit` with a feature that is not a Fillet, Chamfer or
Shell is refused naming the three.

With `"reduce"`, a size that does not fit is replaced by the size that
does, and that size is what the document stores: the feature's `radius` is
7.49, and nothing remembers 10. The response says so, `reduced: {param,
asked_mm, built_mm, basis}`, beside `regen`. A size that fits is built as
asked and answers no `reduced`.

This is not a substitution in the sense CLAUDE.md refuses. The caller
asked for it on this call, the answer names both numbers, and the document
holds the number that was built, so it never loads looking right and being
wrong. For the same reason it is refused where it could not keep that
promise:

- a size given as an expression (`radius_expr`), which a number would have
  to overwrite;
- inside a `batch`, whose regeneration is deferred and has no built body
  to measure;
- when nothing fits.

It is not stored, so the format does not move, and regeneration never
reduces anything: a later edit that makes 7.49 too large breaks the
feature, which then says the new size. The GUI's form of it is the "Use"
button.

## Stages

- **A. Measures, the search and the report** for Fillet and Chamfer: the
  sentence, `fits` in `regen` and in the refusal's `diagnostic`.
- **B. The button** in the tool window and the feature editor.
- **C. Bounds that refuse before the build**, a kind at a time.
- **D. Shell.**
- **E. `fit: "reduce"`.**

## Files

- New `src/core/BlendFit.{h,cpp}`; `src/core/Measure.{h,cpp}` (dihedral,
  width across an edge, least curvature radius).
- `src/core/ElementHistory.{h,cpp}` (the messages),
  `src/core/features/FilletFeature.{h,cpp}`,
  `src/core/features/ShellFeature.cpp`.
- `src/core/Feature.h` (`FitHint`): one header edit, batched with the
  rest.
- `src/remote/RemoteHelpers.h` (the `regen` row),
  `src/remote/RemoteModeling.cpp` (the refusal, `fit`, the `MethodDoc`
  rows), `src/remote/RpcErrors.h`, `python/lobole/errors.py`; `rpc.py` and
  `rpc-surface.json` regenerated.
- `src/ui/PreviewFeature.cpp`, `src/ui/FeatureEditor.cpp`.
- `tools/action_matrix.py` (its self-test plants the old sentence).
- Tests: `tests/test_fillet.cpp`, `tests/test_shell.cpp`,
  `tests/regen_honesty_smoke.py`, `tests/refused_calls_smoke.py`, a GUI
  test.
- Docs: docs/modeling/README.md#refused-on-the-bodies and
  #a-reshaping-feature-keeps-only-a-valid-solid,
  docs/scripting/rpc.md#what-a-response-carries and #structured-errors,
  docs/ui/tool-sessions.md#the-tool-window.

## Verified by

Each case is checked by what the answer is for: the size reported is sent
back through `add_feature`, must build a valid solid of the closed-form
volume at that size, and 2 % above it must be refused.

- An 80×15×20 box, both vertical edges of one end, r10: refused naming a
  radius in (7.4, 7.5]; at it, V = 24000 − 2(1 − π/4)r²·20.
- A cylinder r10 h30, an end edge at r12: names at most 10. A 10 mm plate
  with a through hole, the rim chamfered 10.5: names at most 10. A 20 mm
  cube, three edges of one corner at r20: names a radius under 20.
- An L-bracket's inside bend past the thinner leg: the bound's sentence
  names the face.
- A blend whose limit is below every bound (a round ending tangentially in
  a cylinder): the search's size builds, and the sentence does not claim
  the bound.
- The same failing fillet regenerated five times, and asked again at a
  larger radius, builds its candidates once (counted in the test).
- A cost check, in CPU time: a failed fillet on `plate_100_holes.step`
  answers within ten times one build of it, search included.
- `regen_honesty_smoke`: `set_variable` takes a radius past its limit and
  the response's `regen.failed[0].fits` holds the size; setting the
  variable to it clears the failure.
- `refused_calls_smoke`: the refused `add_feature` carries the
  `diagnostic` and changes nothing; an unknown `fit`, `fit` on an Extrude,
  `fit: "reduce"` with `radius_expr`, in a `batch` and where nothing fits
  are each refused and change nothing.
- `fit: "reduce"` on the 80×15×20 box: the stored radius is the size
  built, `reduced` names 10 and it, the volume is the closed form, a save
  and reopen gives the same body, and the same call at r5 answers no
  `reduced`.
- Shell: a 40×30×10 box with a 1.5 mm bottom round shelled 2 from the top
  reports a wall that builds; shelling at it is valid.
- GUI `fillet/too_large_offers_the_size_that_fits`: the tool window shows
  the sentence with Accept disabled, a click on `###tw_fit` puts the value
  in the field, the preview builds and Accept commits one undo step; with
  the radius bound to a variable there is no button.
- `tools/ctest -L reference`, the headless and GUI suites, and the action
  matrix: no pass becomes a refusal, and `--profile` shows what a refused
  `edge.fillet` now costs.
- Once kernel-case-corpus.md's runner is in, its `refused` fillet and
  chamfer cases compare the size their `refusal` states with `fits_mm`.
