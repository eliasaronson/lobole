# The tube system — from a frame to an order

Bent tube is half-built. `Curve3D` `bent_path` is a centreline as a bender is
programmed with it, `Pipe` sweeps a section along one, and `tube_schedule`
reads back the cut length and, per bend, the angle, the former, the free
straight and the roll between bends. A gas grill's lid handle came out of it
correctly.

A Formula Student chassis does not. Forty-odd members, most straight, several
bent, every one coped to its neighbours, and then ORDERED: a cut list to a
supplier, a bend schedule to a bender, a solid per member to a tube laser.
This plan is that half.

## What a spike found, before any of it was designed

A three-member node was built and coped — a main hoop bent twice on an R76.2
former with two braces landing on it — to see where it stops. The geometry
held: coping a brace by subtracting a solid copy of the hoop gave valid
B-Reps at 90°, 60°, 45° and 30°, and every member stayed its own body, which
is what `export_step {body}` needs. What stops is everything after it.

**The cut list reads the feature list, not the bodies.** Three failures, one
cause:

* A frame modelled as one side and MIRRORED reports one tube for two bodies.
  A symmetric chassis ships half its order.
* The node reported five tubes for three members: a boolean consumes both its
  operands, so each cope needs a throwaway copy of the neighbour, and those
  copies are still curves in the part. The list tells the shop to build two
  more main hoops.
* There is no quantity. Four identical braces are four rows or one, depending
  on how they were authored.

**The length is the node-to-node centreline, not the finished member.** A
Ø19.05 brace coped into a Ø25.4 hoop:

| joint angle | schedule says | metal actually |
|---|---|---|
| 90° / 60° / 45° | 400.00 mm | 391.60 mm |
| 30° | 400.00 mm | 391.10 mm |

Eight to nine millimetres long per coped end. Equal-diameter joints come out
exactly right, because the cope's heel lands on the node — which is the
coincidence that would hide this until the tubes arrived.

**Coping is quadratic.** The neighbour must be rebuilt as a disposable cutter
for every cope: a four-member node rebuilds the run four times. A forty-member
frame is a few hundred features of bookkeeping, and every throwaway pollutes
the cut list.

**There is no manufacturability rule.** The only one in the build is a
hard-coded `radius < 1.5 × diameter` "tight" flag on `tube_schedule` — a
literal, in a project whose defaults are derived. Nothing knows the material,
the wall, the dies the bender owns, or how much straight it needs to clamp.

## T1 — the declared process: `tube::TubeRules` and `check_tube`

The shape sheet metal already proved, and the reason W6 was re-decided
(`docs/peer-survey-plan.md`): a manufacturability check belongs to a declared
process, beside the feature that declares it. `sheet::SheetMetalRules` names
the material and the gauge, so `check_sheet_metal` can state a minimum radius
instead of guessing one. Tube gets the same.

`tube::TubeRules`, per part, mirroring `SheetMetalRules` field for field
where the meaning carries over:

```
material      a CADMaterials slug; required
size          a stocked designation ("25.4x2.4"), required — OD and wall
outerDiameter, wallThickness   resolved from the size at set time
formerRadii   the die set: the radii this bender owns (empty = any radius)
minStraight   between two bends, and before the first: what the clamp needs
              (0 = derived, 2 x OD, which is the common shop rule)
tailLength    gripping length past the last bend (0 = derived, 1 x OD)
```

`tube::Sizes()` / `tube::SizesFor(material)` / `tube::LookupSize()` are the
gauge table's counterpart, stocked sizes per material, and `tube::Validate()`
refuses an unknown material, an unstocked size, a former under what the
material and wall take, and a die set that contains one.

`tube::MinBendRadius(material, od, wall)` is the rule the shop cares about,
and it is about D/t, not D: a thin wall collapses where a thick one bends.
`tube::WallThinning(od, wall, radius)` and `tube::Ovality(od, wall, radius)`
report what the outside of the bend loses, which is what a rules committee
asks a Formula Student team to show.

`check_tube(part)` → `{violations[], bendsChecked, membersChecked, notChecked}`
in the shape `check_sheet_metal` returns, with these kinds:

```
bend_below_minimum_radius     R/D and D/t against the material
radius_not_in_die_set         a former nobody owns — naming the ones they do
straight_below_clamp_minimum  two bends too close to clamp between
tail_below_gripper_minimum    nothing to grip past the last bend
wall_thinning_over_limit      the outside of the bend, against a stated limit
```

Refusing by name is the point: "R45 is not in this bender's dies; it has R38
and R57" is an answer a shop can act on, and it is what this project does
everywhere else.

Files: `src/core/tube/Rules.{h,cpp}`, `src/core/tube/Check.{h,cpp}`,
`src/remote/RemoteTube.cpp` (+ `MethodDoc` rows), `src/core/Document.h` (the
`std::optional<tube::TubeRules>` beside `m_sheetMetal`),
`src/core/io/Serialize*.cpp`. **Format change** — a bump, a golden at that
version, and a row in `docs/versioning/format.md`.

## T2 — a `Cope` feature, so a joint is one move and not three

```
Cope {member_id, against: [feature_id, ...], gap?: 0}
```

Subtracts each named neighbour's OUTER solid from `member_id` and leaves the
neighbours alone. `gap` opens the cope for the weld's root.

It is the enabling piece. A boolean consumes its operands, which is right for
a boolean and wrong for a joint: at a node every member is coped to every
other and they all survive. With `Cope`, a node is one feature per member
instead of one throwaway copy per pair, and nothing lands in the cut list that
is not a part.

It also makes the joint legible: a `Cope` says these members MEET, which a
subtract does not, and that is what a weld record will later hang on
(`docs/manufacturing-gaps.md`).

Files: `src/core/features/CopeFeature.{h,cpp}`, `src/core/io/SerializeFeatures.cpp`
(+ the `kFeatureTypes` row), `tools/designer_mcp.py`. **Format change** — a
new feature type, same gates as above.

## T3 — the cut list comes from the bodies

`tube_schedule` walks `CurveFeature`s. It must walk BODIES and find each one's
path by its provenance (`VisitFeatureRefs` back to the `Pipe` that swept it),
which fixes the mirror, the cutters and the quantity in one change: a body
that exists is a member, a curve that no longer has a body is not.

Each row gains what an order needs:

```
member        the body's name — and the id a label is etched with
quantity      identical rows collapse, and say how many
material, od, wall
cut_length    the DEVELOPED length of the metal, cope to cope — not the
              node-to-node centreline, which is 8-9 mm long per coped end
bends[]       as today: angle, former, straight before, roll
```

`cut_length` for an uncoped member is the centreline developed length it
already reports. For a coped one it is measured off the member's own body
along its path, which is the only number a shop can cut to.

Files: `src/remote/RemoteInspection.cpp` (+ its `MethodDoc` row),
`tests/headless_smoke.py`.

## T4 — the order leaves the building

* `export_cut_list {path}` — CSV, one row per member: id, quantity, material,
  size, cut length, bend count. The BOM's columns where they carry over
  (`src/ui/Bom.cpp`), because a shop reading two files from one project should
  not meet two spellings of "material".
* `export_bend_schedule {path}` — CSV, one row per BEND: member, bend number,
  distance, angle, roll, former. That row order is what a bender is
  programmed from, and it is not the cut list's shape.
* `export_step {path, members: true}` — one solid per member, named by the
  member id, which is what a tube laser takes. `export_step` already takes a
  `body`; this is the batch a forty-member frame needs. (2026-09-22: it
  shipped as `export_members {dir}`, docs/chassis-shop-plan.md S2, and each
  file holds one product named after its member since X4,
  docs/interchange/step-export-plan.md.)
* Stock packing: `cut_list` reports how many six-metre sticks the frame needs
  and what falls off the end, because "how many do I buy" is the question a
  cut list exists to answer. First-fit decreasing over the stocked length,
  with the saw kerf taken off each cut — a small, exact 1D pack, not the
  nesting workstream `docs/cam/stock-and-origin.md` defers.

Files: `src/core/tube/CutList.{h,cpp}`, `src/remote/RemoteTube.cpp`,
`src/remote/RemoteProject.cpp` (the STEP batch).

## Order, and what each stage is verified by

T1 first: it is standalone, it is the W6 re-decision made real, and the
chassis needs to declare its tube before it builds one. T2 next, because
nothing can be modelled properly without it. T3 depends on T2 existing (the
cut list's correctness is only visible once copes are real), and T4 on T3.

* T1 — a `[tube]` unit suite over the tables, and `check_tube` finding a
  planted violation of each kind on a real bent member.
* T2 — a cope at four joint angles and two diameter ratios, asserting the
  neighbour survives and the member's volume drops by the intersection.
* T3 — the spike's own failures as assertions: a mirrored member appears
  twice, a consumed cutter does not appear, and a coped member's `cut_length`
  equals the metal (the 391.6 mm above, not 400).
* T4 — a round trip: build a frame, export, and read the CSV back to the same
  numbers `tube_schedule` reports.

And the whole of it is verified by the thing it exists for: a Formula Student
chassis that produces an order a shop could work from
(`docs/chassis-round-plan.md`).

## As built

T1–T4 shipped, format **v119**, and the chassis that accepted them is
`generators/chassis/` ([the round](../chassis-round-plan.md)).

**T1 landed differently in one place, and the chassis is why.** `TubeRules`
was written as sheet metal's counterpart field for field, including a SIZE —
and that is the one rule that does not carry over. A sheet part is cut from
one blank, so a per-feature gauge would let a body be two thicknesses at
once. A tube frame is several sizes at once *by design*: the chassis is
25.4 x 2.4 in its hoops, 25.4 x 1.65 in its side impact structure and 19.05
in its tertiary members, and a rulebook is what says so. The rules carry the
MATERIAL and the BENDER; each member's Pipe carries its own stock; and
`check_tube` judges each member against its own, which is also where
`size_not_stocked` comes from. Before that, 22 of the chassis's 32 members
were being ordered in the wrong tube and nothing said so.

**The cut length is measured, and measuring is where the bugs were.** Three,
each found by a number that did not add up:

* A bent path is a WIRE of five edges. Measuring along the first answered
  15 mm for a 481 mm handle.
* `GeomAPI_ProjectPointOnCurve` returns only points where the distance is
  STATIONARY, and a tube's end face has none on the leg it sits at the end
  of. A two-bend hoop measured 1511 mm of its 2335 mm path — one whole leg.
* The far point of a coped end is the heel of the fishmouth, which lies ALONG
  the cut curve; where its vertices fall is a matter of how the boolean split
  it. Two mirrored braces came out 0.77 mm apart. Sampling edges by
  deflection brings them to 0.007.

The lesson is the round's: **a measured quantity needs a tolerance and a
noise floor, stated.** Identical pieces group within 0.1 mm — finer than a
saw, coarser than a boolean's residue — and report the longest of the group,
so cutting to a row can only leave a member long. A cope smaller than a
micron is arithmetic, not a cope.

**T2's refusal had to learn to tell two failures apart.** A cope that removes
nothing is either two members that do not reach each other or a TANGENT cut —
two tubes of one diameter whose axes meet exactly, which a boolean answers by
handing back what it was given. The second is not a corner case in a frame:
it is what every equal-diameter joint drawn to its node looks like. A root
gap of 0.05 mm makes the surfaces cross, and a real cope is cut a little open
for the weld anyway.

## Not this, and why

**A weld is still not a thing.** A cope says two members MEET, which is the
hook a weld record would hang on — a length, a size, a symbol, a table — and
`docs/manufacturing-gaps.md` keeps the argument. What exists today is the
dock round's convention: an overlap between two members IS the weld, and the
chassis reports 63 of them.

**Members are not exported one STEP file per member yet.** `export_step`
takes a `body`, so a generator loops; the batch a forty-member frame wants is
a convenience, not a capability. (2026-09-22: superseded — the convenience
is `export_members {dir}`, docs/chassis-shop-plan.md S2.)

**`contact_report` is still slow on a frame.** An oriented-box prefilter took
the chassis from 84.5 s to 68.7 s, and what is left is 100 pairs of fuzzy
booleans at about 0.7 s each. That is a performance round of its own, and it
is why the generator asks for a longer timeout rather than pretending the
question is quick.
