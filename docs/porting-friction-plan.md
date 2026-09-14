# Porting an outside design: what got in the way

**A, D and E are built; B is built except `compare_geometry`; C is built as
far as the SDK reaches.** What each one turned out to need — and the two
items that dissolved on contact — is recorded against it below.

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

**`compare_geometry` alignment is NOT built** and is what remains of B.

**Still to verify** the way that matters: rebuild the braces from loops and
watch the ring brace stop being a re-draw at 0.77x.

Touches the section/inspection RPC and its handler, `compare_geometry`,
`tools/designer_sdk.py`, and a case in `tests/` that reads a known part's
loops back.

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

The generator now carries four helpers that exist solely to work around this:
one that names an axis instead of a quaternion, one that probes a mate's
offset sensitivity and solves it, one that turns a mate over until a face
looks the right way, and one that checks a panel did not move and tries the
alternatives. That is scaffolding around not being able to state the intent.

**The fix.** A verb that says what the joint IS, and lets the app resolve it:

    asm.seat(moving_face, onto_face, align=<frame pair>)

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
is the remaining slice, and it wants the app-side verb this section proposed:

    asm.seat(moving_face, onto_face, align=<frame pair>)

Touches the mate RPC, `tools/designer_sdk.py`, `generators/smart_speaker/`.

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
- The two clash reads are named the same thing. `interference` returns its
  findings under `pairs`; `contact_report` returns the same kind of thing
  under `clashes`. Reading the wrong one yields `[]` and a gate that passes
  for ever. One name, with the other kept as an alias that warns.
- `interference`'s receipt says `claim: "no pair interferes"` with `verdict:
  "fail"`. The claim does not invert with the verdict, so a green-sounding
  line means its opposite. State the claim as what was found.
- `asm.check` is what the modelling-workflow prompt points at for "is this
  assembly right", ahead of the raw sweeps.

**Verified by** the smart_speaker generator's `verify.py` shrinking to a call
to `asm.check(allow=...)`, and by `import_step` round-tripping a part's
placement: export at a known position, re-import with `keep_origin`, assert
the bounding box did not move.

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
