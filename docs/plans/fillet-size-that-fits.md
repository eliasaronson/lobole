# The size that fits: bounds that refuse, Shell, and `fit: "reduce"`

A Fillet or Chamfer that does not build already says the size that does
(docs/modeling/README.md#the-size-that-fits): the measures at the picked
edges, the bounds they give, a search of a fixed number of builds the least
bound seeds, the sentence, `fits` on the `regen.failed[]` row and the
refusal's `diagnostic` (`code: does_not_fit`), and the "Use 7.49 mm" button
(`###tw_fit`). Three things are left.

## 1. Bounds that refuse before the build

Today a bound only seeds the search: the first candidate sits just under
it, and when that builds it is the answer. A size past a bound is still
built, fails, and is searched.

`FilletFeature::WhyRefusedOn` asks the bounds (`fit::BlendBound`,
src/core/BlendFit.h), so a size past one is refused in milliseconds by
`add_feature`, `modify_feature` and Accept alike, with the bound's fact as
the sentence ("fillet radius 10 mm does not fit: face 'F' is 15 mm wide
between the two picked edges, so the rounds meet at 7.5 mm") and
`basis: "bound"` in `fits` (FitHint already carries the field).

A bound kind (the face's width, the width between two picked edges, a
concave face's curvature, a convex rim's radius) refuses there only once
the kernel-case corpus's limit ladders and an action-matrix sweep show
OpenCASCADE builds nothing past it; until then it only seeds the search.
The matrix's ratchet holds this: a pass that becomes a refusal fails the
run.

Files: `src/core/features/FilletFeature.cpp` (`WhyRefusedOn`),
`src/core/BlendFit.{h,cpp}` (which kinds refuse), `tests/test_blend_fit.cpp`.

## 2. Shell

`WallObjection` (src/core/features/ShellFeature.cpp) measures the body
under each open face and says "A wall under 4 mm (2 mm, say) fits". A wall
that passes it and that the offset builder then refuses (an inner round
smaller than the wall, a rib thinner than two walls) gets the builder's
sentence and no size.

That wall is searched the same way on `thickness` (`fit::LargestThatBuilds`),
inside the Shell's one deadline, the first candidate half the body's
thinnest wall (`ThinnestWall`). An offset costs seconds where a blend costs
milliseconds, so the search is four steps, and on a body whose first draw
used most of the budget it reports the bracket it has. The message ends
"the largest wall that fits is 0.7 mm" beside the builder's reason, and the
Shell carries a `FitHint` with `param: "thickness"`, so `regen`, the
refusal's diagnostic and the tool window's button follow with no change of
their own.

Files: `src/core/features/ShellFeature.cpp`, `src/core/BlendFit.h` (the
step count as a parameter), `src/ui/FeatureEditor.cpp` (`DrawFitButton` for
a Shell's thickness), `tests/test_shell.cpp`.

Verified by: a 40×30×10 box with a 1.5 mm bottom round shelled 2 from the
top reports a wall that builds; shelling at it is valid.

## 3. `fit: "reduce"`, on the call

`add_feature` and `modify_feature` take `fit`, beside `feature`: `"exact"`
(the default, what is built) or `"reduce"`. Any other value is refused
naming the two; `fit` with a feature that is not a Fillet, Chamfer or Shell
is refused naming the three.

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

Files: `src/remote/RemoteModeling.cpp` (the `fit` key, its `MethodDoc`
rows, `reduced`), `rpc.py` and `rpc-surface.json` regenerated,
`tests/refused_calls_smoke.py`, docs/scripting/rpc.md.

Verified by:

- `refused_calls_smoke`: an unknown `fit`, `fit` on an Extrude, `fit:
  "reduce"` with `radius_expr`, in a `batch` and where nothing fits are
  each refused and change nothing.
- `fit: "reduce"` on an 80×15×20 box, both vertical edges of one end at
  r10: the stored radius is the size built, `reduced` names 10 and it, the
  volume is V = 24000 − 2(1 − π/4)r²·20, a save and reopen gives the same
  body, and the same call at r5 answers no `reduced`.

## The kernel-case corpus

Its refused fillet and chamfer cases could compare the size their
`refusal` states with `fits_mm`, but the runner keeps only the message: the
translator (`tools/kernel_cases_translate.py`, `Refusal`) drops the failed
row and the diagnostic, and `expected.json` has no numeric key for the size
(an unknown key is a `CorpusError`). It needs a `fits_mm` key in
`expected.json` (graded with the case's tolerance), and `Refusal` to carry
the `fits` object from the diagnostic or the failed row.
