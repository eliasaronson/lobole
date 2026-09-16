# What a bicycle cost, and what to do about it

Building `generators/bike/` took 4.6 hours of work. Measured from the session
log, a third of it was waiting for the same 4-minute loop, and a third of the
failed iterations were neither design defects nor kernel trouble — they were
the app answering a question it had not been asked, answering a question it
should have refused, or spelling a noun a fourth way.

| | |
|---|---|
| waiting on runs | **96 min** of 279 |
| of the 54 run outcomes: a check caught a REAL defect | 17 (31%) |
| kernel/boolean fragility | 16 (30%) |
| the API's shape — wrong parameter, wrong verb | 16 (30%) |

Four stages, in order of what they cost.

## 1. A cheap question must not pay for an expensive one

`check_geometry` on a 36-spoke wheel takes **96 seconds**. The validity check
it is named after takes **0.2** — measured per body, all three bodies of the
wheel together. The other 95.8 s is this, at the end of the handler:

```
if (bodies.size() >= 2 && only < 0)
  const auto comps = BodyComponentNames(part, 1e-3, 1.0);
```

the floating-bodies warning, which runs an **exact** `Measure::ContactBetween`
— an OCC boolean plus `BRepExtrema_DistShapeShape` — on every pair of bodies
whose bounding boxes are within a millimetre. A wheel's rim, spoked hub and
tyre are concentric, so all three pairs qualify, and each is an exact contact
against a 36-spoke solid. Two wheels are 190 s of a 280 s generator run, and
that run is the loop the round went round nineteen times.

The warning is worth keeping — it is opt-OUT by design, and it earns that.
What it must not do is cost 480 times the check it rides on, silently.

**The fix.** The warning is a yes/no, not a measurement, so it should use the
triangles: `Measure::MeshGap`, the same answer `interference mode=mesh`
decides a clash with, with its deflection caveat carried into the response
like every other mesh verdict here. Where even that is too big, skip the pair
and SAY so — `contact_report` already has the vocabulary for a bounded sweep
("the sweep was TRUNCATED, so this is not a clean bill of health").

Files: `src/remote/RemoteInspection.cpp` (`BodyComponentNames`, and the
`check_geometry` handler), `src/core/Measure.h` if the mesh path needs a
body-set entry point.

**Verified by**: `check_geometry` on the bike's front wheel, measured before
and after; the existing floating-body assertion in `contact_smoke.py` still
firing; and two new cases beside it for the pairs that actually cost the time
— bodies whose bounding boxes overlap completely, one touching (a rod in its
sleeve's bore, which must read as connected) and one not (a thinner rod
floating in the same bore, which must not).

## 2. An invalid body must not answer a measurement

The most expensive single hour of the round was a clash that did not exist:
`interference` reported **17,297 mm³** of the rear cog buried in a frame whose
own bounding box does not reach it, and a probe disc 40 mm outside the frame
read as 98% inside it. The frame's body was invalid — OpenCASCADE calls it
"bad wire nesting" — and every boolean asked of it returned confident
nonsense. `check_geometry` knew. Nothing else asked.

This is the rule the app already applies to input — an enum it does not
recognise is REFUSED, never quietly replaced — applied to output. A
measurement taken on an invalid solid is the same class of lie: plausible,
undetectable, and wrong.

**The fix, at the source rather than at the question.** `CADShape::IsValid`
is `!IsNull()` — a null check, not a validity check — so the boolean that
made the mess reported success. Validity is cheap (0.2 s for the worst body
in that bicycle, measured above), so the BOOLEAN checks its own result and
FAILS, naming OpenCASCADE's reasons. That puts the error where a person can
act on it — the feature goes red, the generator stops — instead of at the
far end of whatever question is asked next, and it means every later
measurement is safe by construction rather than by a gate on each caller.

Gating the measuring RPCs as well would need a per-body validity cache to
stay affordable in an 800-pair sweep. It is worth doing for bodies that
arrive some other way — an imported STEP, a sweep — and is NOT in this round.

Files: `src/core/features/BooleanFeature.cpp`, `tests/regen_honesty_smoke.py`
(the contract it extends: a mutating RPC that breaks the model says so).

**Verified by**: the failing configuration itself — a rear dropout with two
bowed stays landing on it, bored in one pass — reproduced in the smoke test,
which passes with the check and fails without it. Written with straight
cylinders instead the same test builds cleanly and asserts nothing, so the
geometry is faithful on purpose.

## 3. The default that assembled a different bicycle

`add_mate`'s `flipped` defaults to TRUE — right for two touching FACES, wrong
for two seat FRAMES, and the RPC's own help says so. Left at its default every
mate in the bicycle still "assembled", and the result was a different
bicycle: the fork turned end for end about its own steering axis, carrying the
front wheel 700 mm into the air; the seatpost, saddle and carrier hanging
underneath; the chain and its guard mirrored onto the wrong side. Nothing
errored.

**The fix.** The SDK's assembly verbs — `fasten`, `revolute`, `slider`,
`cylindrical`, `planar`, `ball` — pass `flipped=False` when both connectors
are FRAMES, and leave the RPC's documented default alone for face anchors and
raw callers. This is the agent's surface, and the agent is who got it wrong.

Files: `tools/designer_sdk.py`.

**Verified by**: re-running every generator that fastens frame to frame and
gating on ITS OWN checks, not on mine — `tests/generators_smoke.py`, the
watch, and the bicycle. A generator that was quietly relying on the old
default will say so by failing its own clash or connectivity gate.

## 4. One name for one thing

Sixteen of the round's fifty-four failed iterations were the API's shape. The
refusal messages are good — they name what is accepted, which is why each cost
one round trip rather than a debugging session — and most of the sixteen were
guessed methods and guessed values that no gate can prevent. One class can be:
being made to learn a second spelling of a noun you have already learnt.

Scanned over all 252 methods, the vocabulary follows a rule already — one
worth writing down, because no single method shows it:

- `id` is the id of the thing the method is NAMED after: `remove_mate(id)`,
  `set_cam_op(id)`, `set_occurrence_visible(id)`. `set_mate(id)` is not an
  inconsistency; it is the rule.
- `<noun>_id` is a reference to something the method is not about:
  `set_material(feature_id)`, `frame(feature_id)`.
- `sketch_id` stays narrower than `feature_id` on purpose — it says the
  feature must be a Sketch, and a caller passing an extrude fails at the call
  instead of somewhere further in.

Two methods are out of step, not four: **`delete_feature`** took `id` where
all eight of its siblings take `feature_id`, and **`set_rollback`** offered
only an `index` — the one thing a caller holding a feature does not have,
since it has to count the tree to roll back to the thing it just made.

**The fix.** Both accept `feature_id`, keeping what they took before, and a
gate writes the rule down: a method that resolves a feature takes
`feature_id`, checked against the keys its handler actually reads.

Files: `src/remote/RemoteModeling.cpp`; a new `tests/rpc_names_gate.py`.

**Verified by**: the gate itself, plus the existing `rpc_params_gate.py`
(which keeps the `describe` rows honest against the handlers) staying green.

## What this plan does NOT do

A `Frame` feature — members along paths, mitred and coped where they meet, and
a cut list — is the other thing this round argued for, and it is a bigger
piece than all four stages above put together. The evidence for it is real
(fourteen tubes cost 80 features and a third of the round's failures were
booleans meeting at tangents) and the recipe is proven in
`generators/bike/bike_shop.py`'s `Weldment`. It gets its own plan.
