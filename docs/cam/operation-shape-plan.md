# `cam::Operation` — what to do about the union

Hardening item 9 (docs/cam/hardening-round.md). `Operation` carries 25 fields
and about ten of them exist for one op type each, so a surfacing op holds
tabs it refuses and a drill holds a corner relief it has never heard of.

The item as filed named FreeCAD's answer — tabs, dogbones, boundaries and
lead-ins as separate *dressup* objects an operation is decorated with, so an
op that cannot take tabs is simply never wrapped and there is nothing to gate.
This plan exists because that answer does not survive contact with this code,
and because the thing actually worth buying turns out to be much cheaper than
either version of the restructure.

## What the dressup model assumes, and why it is not true here

A FreeCAD dressup wraps a finished `Path` and **rewrites it**. That is what
makes it composable and reorderable: each one is a function from a path to a
path, and the operation underneath knows nothing about any of them.

Two of the four candidates here are not that.

- **Corner relief** (`CornerRelief`, R4) is applied to the OFFSET RING, before
  a single move exists — `CamStrategies.cpp:434`, and through `WallRelief` for
  a pocket. It needs to know where the material's sharp inside corners are,
  and the emitted moves cannot say: by then the corner is a fillet of the
  cutter's radius and indistinguishable from one the designer drew. A dressup
  over the path would have to re-derive the boundary the strategy already had.
- **Lead arcs** (`leadArcs`, R5) set the ENTRY of each loop, and the entry is a
  ramp — "the ramp is the entry, so there is no lead in: the tool arrives at
  the wall descending, already moving along it". The lead is chosen while the
  loop is being emitted with the ramp, not bolted onto it afterwards.

**Tabs** genuinely could be a path rewrite: `EmitLoop` raises Z over four spans
of a closed polyline, and a pass over the moves could do the same. **Boundaries**
are not a field on `Operation` at all — this app picks targets instead.

So the honest count is: one of the four is a dressup, one is not a field, and
two are inputs to the offset. A wrapper model built on that is a wrapper around
one thing, which is not a model.

## The three options, and what each buys

**A. Dressup wrappers.** ~110 call sites for tabs/relief/leads alone, a format
change, and an RPC change: `add_cam_op(tabs={...})` becomes a second verb.
That last part is what settles it — `generators/waveguide_job.py` passes
`tabs={count,width,height}`, and so does every job anyone has written. Buys:
tabs become impossible on an op that cannot take them, instead of refused.

**B. Per-type parameter structs** — `std::variant<ProfileParams, …>`, or named
sub-structs held by value. The discriminant becomes real and a reader sees the
grouping at the point of use. ~200 mechanical edits, a nested format, no
behaviour change, and every read site grows a `std::get_if`. Buys: the same
thing the capability table already answers, spelled in the type system.

**C. Enforce the table instead of dissolving the union.** The table
(`CamNames.h`, `OpField`) already states, per type, exactly which fields that
type's generator reads. It was traced by hand through the helpers — the file
says so, and says which three traps that hand-tracing had to avoid. Nothing
checks it. So: for every op type, perturb every field its row does NOT claim,
regenerate, and assert the toolpath is byte-identical. A generator reading a
field it does not declare moves the path, and the test names the field.

## Recommendation: C, and say why A is not deferred but rejected

The defect behind item 9 is not that the fields are in one struct. It is that
nothing stops a strategy reading a field that is not its own, and that the
document saying which are its own is prose maintained by hand. C closes that
for the price of one test, changes no format and no RPC, and turns the
capability table from documentation into an enforced invariant — the same move
`test_conventions.cpp` makes for the renderer lists and the unit suffixes.

A and B both cost a format change and a wide diff to buy a property C gives
for nothing, and A additionally breaks the verb every existing job is written
against. The union is not the problem; the unenforced claim is.

What C does NOT buy, stated plainly: the struct stays 25 fields wide, and a
caller can still SET a field its type ignores. The RPC refuses that already
(`ApplyOpFields`, RemoteCam.cpp:466) and the panel does not offer it; the
in-process API does not, and will not after this.

## Built

C, as `tests/test_cam_capability.cpp`. Two things it found or forced that are
worth recording.

**It caught an under-claim on its first run.** `GenerateOpFramed` split an
operation into a rough and a finish pass on `op.finishTool != 0` alone, and
never asked the table. A surfacing type's row claims no `Finish` — the RPC
refuses `finish_tool` on one by name and the panel never offers it — but an op
carrying one from any other route (a loaded file, the in-process API, a test)
quietly grew a second full-depth pass of a strategy that has no wall to
finish. The check now asks the table. That is the whole argument for this test
in one bug: three places already agreed surfacing has no finish tool, and the
one place that does the work was not one of them.

**Poke in BULK, bisect on failure.** Setting each undeclared field on its own
means ten generates per type, and adaptive clearing costs about five seconds a
call: 422 s, more than the entire `[slow]` set. Setting every undeclared field
AT ONCE answers the same question in one generate — if the path is identical,
none of them was read — and the per-field loop runs only when that fails, to
name the culprit. 11.5 s, which is what buys the right to leave it out of
`[slow]` and in the fast loop, where a table being edited by hand wants it.

The fixture needs stating too, because two of its three properties are load
bearing and neither is obvious. Surfacing needs curvature WITH AREA SEEN FROM
ABOVE: a waterline over a flat top has no laps and generates nothing, and a
cylinder's wall is refused as edge-on to the spindle — either way the loop
would pass without testing a thing. And the whole thing runs a 10 mm cutter on
a half-size part, because the clearing strategies cost passes.

## What to build

One test, `tests/test_cam_capability.cpp`:

- A job with one part and one operation, built per type from the same fixture
  so every type cuts the same geometry.
- For each type: generate, then for each `OpField` the row does NOT claim, set
  that field to a value distinguishable from its default, regenerate, and
  compare the moves exactly. Any difference is a failure naming the type and
  the field.
- The fields that are not fields (`FullWidth`, `ClearsInside`,
  `NeedsFlatFloor`) are skipped: they are per-type facts, not inputs.
- A NEGATIVE control, because a check nothing fails is not a check: perturb a
  field the row DOES claim and require the path to move. That also catches the
  fixture going inert — an op that generates nothing passes every equality in
  the loop.

The two deliberate carry-overs in the table (Pocket and EdgeRound claim `Side`
without reading it) stay legal: the test only fails on an UNDECLARED field that
moves the path, so over-claiming is allowed and under-claiming is not. That is
the right asymmetry — over-claiming shows a field in the panel that does
nothing, which is a papercut, and under-claiming is a field that silently
steers a cut nobody thinks it steers.
