# Porting an outside design: what got in the way

**A, B, D and E are built; C is built as far as the SDK reaches, and E's
camera fit is not.** What each one turned out to need — and the three items
that dissolved on contact — is recorded against it below.

Bringing a real Onshape cabinet into this app — six panels, two braces, a
port, four bought assemblies, thirty occurrences — surfaced five things worth
fixing. They are ordered by what they cost, and each one is here because it
was hit, not because it looked untidy.

Two of them are not missing capability at all. `asm.check()` already does
what a whole afternoon was spent hand-rolling, and `keep_origin` already
exists one layer below the RPC that needed it. Those are reachability
problems, and they are cheaper and more valuable to fix than the features
they were mistaken for.

---

## A. A document can lie about being parametric

`Param` is a float that carries its expression, and it is the whole reason a
generator can be written as ordinary arithmetic. Only `_dim` and `_dim3` knew
that. Everywhere else tested `isinstance(x, str)`, and since `Param` subclasses
`float` it took the literal branch:

- `SketchBuilder.circle(d=<Param>)` — every hole any generator drills
- `_uv` — every line, point and polyline coordinate

Both silent. The document builds, looks right, and reports `cached, executed
0` when the variable it supposedly depends on changes; the part rebuilds to
exactly the same shape at any value. A cabinet whose whole outline was Param
arithmetic was frozen this way and nothing said so.

**The fix.** Both call sites are corrected already. What remains is the class:
audit every `isinstance(_, str)` in `tools/designer_sdk.py` and give `Param`
the same standing as a string wherever an expression is accepted. Where the
app has no expression-driven field for that coordinate — a `rect` entity's
corners — a Param falls back to its number rather than raising, because it
HAS one; a bare string still refuses, because it does not.

**Verified by** a new case in `tests/sdk_smoke.py`: build a part whose every
dimension and coordinate is a Param, change one project variable, and assert
the geometry moves. It fails on `_uv` before the fix.

**Built.** The audit found no third site: `cylinder` and `sphere` are already
correct, because `d / 2.0` on a Param composes an expression and `_dim` knows
Param. Writing the test also produced a false alarm worth recording — the
first version declared its variable at PART scope and edited it at PROJECT
scope, so the part correctly did not move, and that looked for twenty minutes
like an app bug in `entityExprs`. It is not: the app evaluates entity
coordinate expressions and hashes them, and a matched-scope edit drives the
geometry.

Touches `tools/designer_sdk.py`, `tests/sdk_smoke.py`.

---

## B. There is no way to measure a solid you did not draw

Porting is mostly reverse-engineering, and the tools stop at scalars.
`section(regions=True)` returns each region's **area and centroid** and
nothing else — no loops, no bounds. So the questions a port actually asks
have no answer:

- what is this brace's outline, so I can redraw it parametrically?
- how wide is this driver at 8 mm depth, so I can size the rebate?

Both were answered by census — counting cylindrical faces by radius, binning
face centroids by depth, taking section areas at a dozen heights and solving
backwards — and one of them was answered wrong twice before a screenshot
settled it. A brace was abandoned and re-drawn to a rule because its shape
could not be recovered; the volume is 0.77x the original and that is the best
available.

**The fix.** Two reads, both on geometry the kernel already has:

- `section` gains the region **loops** (ordered segments: lines, arcs, their
  radii) behind an opt-in, since the area-only form is the cheap one.
- ~~an **extent along an axis** query~~ — **not needed.** Loops answer it:
  section at the depth and read the outline's extent. The W170S comes back
  175.9 x 160.4 across from 4 to 10 mm deep and narrows at 12, which is the
  rebate question answered with the primitive that was already being added.
  One verb, not two.

`compare_geometry` belongs to the same problem and is nearly useless without
one change: it computes IoU without aligning first, so it read 0.333 on a tube
whose volume matched the reference to four figures and was simply 900 mm away.
Align on centroid (or accept a transform) before the boolean, and "is my
re-draw close?" becomes answerable.

**Loops are built.** The ring brace reads back as one region of 25,132.5 mm2
with ten loops whose outer one spans 210 x 320 — the part, off the model
instead of a screenshot. The outer wire is the largest bounding box, not the
one `BRepTools::OuterWire` names: after the boolean that makes a cut face,
that wire is not `IsSame` as any the explorer walks and every loop came back
flagged inner.

**`compare_geometry {align: true}` is built too**, so B is complete: the
re-drawn port tube goes from iou 0.333 to 1.000. Off by default, because a
golden taken AT a position is a real thing to check, and `center_offset`
still says how far it moved.

Both are gated: `section {loops}` in `tests/test_measure.cpp` (a box's cut is
one outer loop of four lines; drilled, two loops, one outer, and an arc that
reports the bore's radius) and `compare_geometry {align}` in `sdk_smoke`.

**Still to verify** the way that matters: rebuild the braces from loops and
watch the ring brace stop being a re-draw at 0.77x.

Touches the section/inspection RPC and its handler, `compare_geometry`,
`tools/designer_sdk.py`, `tests/test_measure.cpp`.

---

## C. A mate does not land where you meant, and cannot be asked

A fastened mate between two frames has four outcomes — which side, and
whether it is turned half a turn about the joint axis — and none of them is
predictable from the call. Measured, in one assembly:

- `flipped=True` seated the woofer correctly and turned the Alpair
  magnet-first at the room. Two mates written identically.
- the offset's sign is per-mate: correcting by `-err` seated the port flare
  and sent the tube to z = -2250.
- a frame built from two bores sits at their **midpoint**, and the two halves
  of a joint are rarely drilled the same depth — 8 mm into a panel's face, 22
  into an edge — so fastening midpoint to midpoint puts a panel 15 mm inside
  its neighbour.
- two dowels at +/-110 are symmetric, so the half-turn is a real ambiguity and
  the right side wall came back upside down.

The generator CARRIED four helpers that existed solely to work around this:
one that named an axis instead of a quaternion, one that probed a mate's
offset sensitivity and solved it, one that turned a mate over until a face
looked the right way, and one that checked a panel did not move and tried the
alternatives. That was scaffolding around not being able to state the intent.

**The fix.** A verb that says what the joint IS, and lets the app resolve it:

    asm.join(moving_face, onto_face, align=<frame pair>)

(`asm.seat` is taken: what shipped is `seat(mate, measure, want)`, which
solves an offset. The app-side verb needs its own name.)

"this face lands on that face, these bolt circles line up" — the app picks the
side, the twist and the offset, because it can see both solids and the caller
cannot. Where a frame is built from bores, it should offer the joint FACE, not
the midpoint of the holes; that is the plane two parts actually meet on and
the only thing both halves agree about.

**Built as far as the SDK reaches**, which turned out to be most of it:
`asm.fasten_in_place` picks the side and the half-turn that leave a part
where its own placement put it, and closes a purely axial residual (the two
frames' drill depths, which is derivable and the only closable case);
`asm.seat` probes an offset's sensitivity and solves it. `_seat`, `_hold` and
`_touch` are deleted from the generator and the cabinet builds identically —
25 mates, one connected component.

Two things did not go, and the plan was wrong to expect them to.
`seating.py` names an axis instead of a quaternion for INITIAL placement,
which is a different job from mating. And `_face_up` answers the one question
neither verb covers: a part whose placement is only approximate has no "where
it was" to keep, so the test has to be which way a FACE ends up looking. That
is the remaining slice, and it wants the app-side verb this section proposed
— under its own name, because `asm.seat` is taken by the offset solver that
shipped:

    asm.join(moving_face, onto_face)

A third instance arrived while bolting the amplifier to the bay floor, which
is worth recording because it is the same problem from a new direction: a
frame built on ONE face has no meaningful X either, so all four quarter turns
are candidates as well as both sides. The solver picked, in turn, a case sunk
8 mm into its own floor, a case hanging underneath it, and a case standing on
edge. Each of those was a search over the same eight outcomes with a
different acceptance test written by hand, because the call could not say
what the joint IS.

### What to build

Not a new verb after all — a new ARGUMENT, because the app already computes
this and only the fastener path can reach it. `ui::JoinAcrossInterface`
(src/ui/FastenerActions.cpp:293) takes two touching faces and reads the five
numbers off the placement in front of it:

    rel = inverse(world_a * f_a) * (world_b * f_b)
    flipped  = rel[2][2] < 0
    rotation = atan2(rel[0][1], rel[0][0])
    dx, dy, offset = rel[3][0..2]

That is the whole of what the four helpers search for, done in closed form.

    add_mate(..., derive: "twist")

**`derive: "twist"`** takes `flipped` and `rotation` from where the two
occurrences currently lie and leaves `dx`/`dy`/`offset` at zero. The joint is
therefore EXACT — the two anchors coincide, which is the point of mating —
and the part is turned the way it already roughly was, instead of landing on
whichever of eight discrete candidates the solver reached first. Refused, not
silently rounded, when the two anchors are not square to one axis: a caller
who asks for the nearest quarter turn between skew frames is asking a
question with no answer, which is the "the two halves of the joint disagree"
case.

`derive` is NOT `fasten_in_place`, and does not replace it: that one keeps a
part where its own placement put it by choosing among eight EXACT joints and
reporting how far the nearest one was. Deriving all five numbers instead
would reproduce the placement exactly and thereby bake a 3 mm error into a
joint that is supposed to be flush. Preserving a pose and stating a joint are
different requests, and the difference is which one gets to be wrong.

**Built.** `assembly::DeriveMateDofs`
(src/core/AssemblyLoopSolve.cpp, beside `MateAdjustFor` whose exact inverse it
is) and `ui::DeriveMateFromPlacement` (src/ui/MateActions.cpp, which resolves
the two connector frames); `add_mate derive:` in
src/remote/RemoteAssembly.cpp; `asm.join` in tools/designer_sdk.py, where
`fasten_in_place` now asks for the derived joint FIRST and falls back to the
eight-way search only when the app refuses. No format change — `flipped` and
`rotation` are fields the mate already had; what moved is who computes them.

`add_mate`'s reply also became `MateJson` rather than `MateToJson` — the same
shape `list_mates` returns, with `rotation_deg` beside the radians. That reply
is now where a caller reads what the app DECIDED, which is a different thing
from an echo of what was sent.

Two helpers are gone from the generator: `_face_up` (the flare's orientation
— the case this section named as the remaining slice) and `_lay_it_down` (the
amplifier's). What replaced each is the assertion it used as its acceptance
test — `_bolts_below`, `_check_laid_down` — which is worth keeping, because
each names something that went wrong on a real run and now fails loudly
instead of being searched around. The cabinet builds identically: 27 mates,
clean under `asm.check`, the flare's throat at -998.51 as the source STEP has
it, and the assembly solve 6.1 s -> ~4 s.

Verified by `tests/test_assembly_solve.cpp` ("DeriveMateDofs inverts the
adjust it is the inverse of", over both sides and five turns, and the refusal
on a tilted frame), `tests/assembly_mates_smoke.py` (the wire form, that a
refused derive leaves NO mate behind, and both refusals), and by the generator
building with the searches taken out.

---

## D. The right verb exists and is not the one you find

Two capabilities were rebuilt badly because the good one was not visible from
where the caller stood.

**`asm.check()`** does clashes, connectivity ("nothing floating"), near misses
under a millimetre, and an `allow` map for contacts that are MEANT to happen.
Every one of those was hand-rolled here: a clash reader that got the key wrong
and returned `[]` on a document with 1,068,565 mm3 of overlap, a connectivity
check added only after eight T-nuts shipped floating in mid-air, and an
`EXPECTED` set of slip-fit pairs that is `allow` with a worse name.

**`keep_origin`** is a parameter of the Import FEATURE, and its own
documentation says exactly who needs it — "what a neighbour part, a fixture in
a shop job" wants. The `import_step` RPC does not pass it through, so every
bought part arrives centred on its own bounding box with the source assembly's
placement lost. That cost an assembly with 47 clashing pairs and every bought
part stacked at the origin.

**The fix.**

- `import_step` accepts and forwards `keep_origin`.
- The whole-assembly `interference` sweep reports its findings under BOTH
  `pairs` and `clashes` — the same list, since it only ever reports clashes,
  and `clashes` is what `contact_report` calls the same thing. Nothing is
  renamed and nothing is deprecated: both names are correct, which is the
  cheapest way to stop a caller reading `[]` off the one that did not exist.
- The receipt's claim wording is left alone. See "Not in this round".
- `generators/README.md` points at `asm.check` — that, not the MCP prompt,
  is where a generator author is standing when they hand-roll it. The MCP
  workflow prompt already said to gate on `contact_report` and be one
  connected component; it was right and is unchanged.

**Verified by** the smart_speaker generator's `verify.py` shrinking to one
`asm.check(allow=...)` — which immediately found two things the hand-written
version had missed: an allowance matching nothing, and a 0.15 mm near miss
where a driver flange had been cut into its ply at exactly flange diameter.

**Gated.** `headless_smoke` exports a box at x=400 and re-imports it twice:
centred it lands on the origin, with `keep_origin` at 405. Only the FEATURE
had a golden (v97); the RPC a bought part actually arrives through did not,
which is how it came to be missing entirely.

Touches `src/remote/RemoteInspection.cpp` and the import handler, the
`describe` rows, `tools/designer_sdk.py`, the MCP workflow prompt.

---

## E. One crash and four sharp edges

**A fillet takes the process down.** On a brace, nine edges blend and the
tenth SIGSEGVs inside `BRepBlend_SurfRstLineBuilder`, reached from
`FilletFeature::Execute`. Repro:
`scratch/smart_speaker/fillet_crash_repro.py`. The tenth edge is 24 mm long
with both neighbours already blended at R8, so 8 mm of face is left to land
on — plausibly not buildable, and a feature that cannot build must REFUSE the
way a bad boolean does. A generator that asks for one bad blend currently
loses the whole session, and the shape is a normal one: chamfer this set of
edges.

The four small ones, all measured:

- **`fit=True` is not aspect-aware.** A 210 x 320 part in the 1280 x 720
  viewport gets its length cropped, which is how a part came to be measured in
  pixels.
- **`app.part()` addresses a tab by name only.** An imported STEP gives thirty
  tabs called `Part 1`; the slot id is the only way to say which, and the SDK
  has no way to pass it.
- **`delete_part` refuses while occurrences exist** with advice for the GUI
  ("right-click one in the assembly's Model panel") and no scriptable path.
- **`Batch` is `Part.batch()`**; on `App` the name falls through `__getattr__`
  to the generated RPC and fails with "missing required parameter 'calls'".
  Also `set_occurrence_visible` takes `id` where its neighbours take
  `occurrence`.

**The crash is fixed** — exact HLR already installed OCC's signals as
exceptions, so that becomes a shared `InstallOccSignalHandlersOnce()` and the
blend calls it. The bad set now returns "the fillet failed on 24 contour(s)"
and the session survives; gated in `sdk_smoke`. The three SDK ones are done.

**`fit=True` is NOT fixed, and the diagnosis in this plan was wrong.** The fit
code is already aspect-aware and its comment names this very cabinet's panel.
It still crops: a 210 x 320 part in 1280 x 720 comes back at distance 416.8
with the target at (1.16, 23.19, -32.47) rather than the part's centre, and
345 mm of visible height should have fitted 320. Something other than aspect
is wrong and it has not been found.

Touches `FilletFeature`/`topo::EdgeBlendNamed`, the camera fit in the view
code, `tools/designer_sdk.py`, the `delete_part` handler.

---

## Not in this round

**`add_occurrence` at scale.** Placing ~210 occurrences with transforms
overran the client timeout even chunked at 40 per batch. But `om615` places
241 occurrences and does not, so this is not a general scaling defect and the
difference has not been measured. It needs a measurement before it needs a
fix.

**The receipt's claim wording.** `claim: "no pair interferes"` with
`verdict: "fail"` reads green at a glance, but it is a hypothesis-plus-verdict
convention used consistently across the receipts, and the summary already
says `FAILED: ...`. Misread, not broken.

**`project_geometry` returning construction geometry.** An extrude cannot use
it ("sketch has no closed profile"), which is correct — projected geometry is
a reference, not a profile. Worth one sentence in the `Derived` and
`project_geometry` rows saying so, since the obvious way to build a part FROM
another part's outline is the way that does not work.
