# Tube frames

A tube frame is a part of many bodies, one per MEMBER: a length of round tube
that is cut, bent, labelled and welded on its own. The app models the members,
the joints between them, the shop they are bent in, and the order that leaves
the building — a cut list, a bend schedule and one STEP per member.
`generators/chassis/` is the full-scale user of all of it.

## Members and their paths

A member is a body made by a `Pipe` whose path is a `Curve3D` of kind
`bent_path`. A bent path is a tube as a bender is programmed with it: `points`
are the CORNERS of the centreline in order, and `radius` (+`_expr`) is the
former it is bent round, measured to the centreline. It builds line–arc–line,
so every bend's angle, former, the free straight before it and the roll
between bends can be read back. A spline fitted through sampled arc points
cannot do that: it bows between points on the straights and has no radius
anything can read (`src/core/features/CurveFeature.h`, `Curve3DKind::BentPath`).

A path that cannot be bent — a straight shorter than the bends at its ends
need, a corner that doubles back — is refused by `add_feature` with that one
reason and nothing is committed, so the `Pipe` a script adds next never runs
on it. A path that breaks later (an edit) still regenerates broken, and the
pipe on it names it ("pipe path '…' failed to build") rather than calling it
the wrong kind of path.

The metal turns SHORT of each corner — on a 152.4 mm former round a right
angle, by R(√2−1) ≈ 63 mm — so anything asking "where does this member run"
must use the evaluated curve, not the corners. `curve_points {curve,
deflection_mm}` returns that polyline with the arc length of each point.

Every member stays its own body and nothing is unioned. Each member's `Pipe`
carries its own `outer_diameter` and `wall_thickness`, because a frame is
several sizes at once.

## Tube rules

`set_tube_rules` declares the SHOP a part is bent in; `get_tube_rules` reads
it back with every derived value resolved (`Document::Tube()`, serialized as
the document's `tube` key):

| field | meaning | unset |
|---|---|---|
| `material` | a CADMaterials slug; required | — |
| `former_radii_mm` | the die set: centreline radii the bender owns | any radius |
| `min_straight_mm` | straight a clamp needs between bends | 2 × that member's OD |
| `tail_length_mm` | straight gripped past the last bend | 1.5 × that member's OD |
| `max_thinning` | fraction of the wall the outside of a bend may lose | 0.25 |

There is deliberately no SIZE. A sheet part is cut from one blank, so sheet
metal's rules carry the gauge; a tube frame is several sizes by design (a
Formula Student chassis is 25.4 × 2.4 in its hoops and 19.05 in its tertiary
members), so each member's Pipe carries its stock and every check judges a
member against its own. The rules drive no geometry: setting them rebuilds
nothing and changes only what `check_tube` may say. `tube::Validate` refuses
an unknown material, a negative straight or tail, a thinning limit outside
(0, 1) and a former under what the stock takes, at the RPC and at load.

The stock table (`tube::SizeTable`) carries `steel`, `stainless_steel`,
`aluminum` and `titanium`, each size designated `ODxwall` ("25.4x2.4") the way
round tube is ordered. `LookupSize` refuses an unstocked size naming the
nearest ones; it never rounds to a neighbour.

The minimum former is set by D/t, not D — a thin wall folds on the inside of
the bend where a thick one bends. `tube::MinBendRadius` takes 2, 3 or 4 × OD
for D/t < 15, 15–30 and ≥ 30, times a material factor (steel 1, aluminium
1.15, stainless 1.2, titanium 1.5), and every bend carries where its limit
came from ("table:steel D/t<15"). The outer wall after a bend is
t·R/(R + D/2) (`WallAfterBend`, `WallThinning`).

## Cope and mitre

```
Cope {member_id, against: [ids], mitre: [ids], gap?: mm (+gap_expr)}
```

One `Cope` per member handles all of its joints. It cuts the member with each
neighbour and leaves every neighbour standing. A Boolean cannot do this: it
consumes its operands, so coping with one needs a throwaway copy of the
neighbour per joint, and every throwaway is a tube in the cut list that
nobody ordered.

- `against` — neighbours whose SIDE the member lands on: a fishmouth, cut with
  the neighbour's OUTER solid, so a hollow neighbour cuts as though it were
  solid.
- `mitre` — neighbours the member meets END TO END, where there is no round
  side to cut into: both tubes are cut on the plane through the joint that
  bisects them, so two tubes at 90° are each cut at 45°. The joint is the
  closest approach of the two centrelines. Both members must be on bent
  paths; two that share no node, or run in line, are refused.
- `gap` opens either cut for the weld root: a cope on the radius, a mitre by
  moving the plane back along the member. A gap against a neighbour that is
  not a tube is refused.

Which cut a joint wants is TOPOLOGY the caller holds, not geometry the
feature reads. A plane through the very end of a tube only shaves a wedge off
it, so an end that will be mitred must be swept LONG — about a diameter past
its node — and once it is, a corner and a tee look alike. Only mitred ends are
drawn long: a cope's saddle reaches only to the node, so an overrun on a
coped end would stand proud of the frame.

A cut that changes nothing is refused, and says which of three things it is:
the two members do not reach each other; the cut lands on the neighbour's
BEND (a torus, thinned by bending, with no straight to fit against — move the
joint clear, and the message names where the bend ends); or the cut is
TANGENT, which is what every equal-diameter joint drawn exactly to its node
is, and a boolean hands back what it was given. A `gap` of 0.05 mm makes the
surfaces cross. A mitre that removes nothing because an earlier cut at the
same end already took that material is accepted.

An overlap between two members is the weld: body-scope `contact_report`
counts it as a connecting clash, so never assert `clashes == []` at body
scope on a welded frame.

## The cut list and the bend schedule

`tube::Members` (`src/core/tube/CutList.cpp`) is the one derivation the
schedule, the CSVs, the member STEPs and `check_tube` all read, so they cannot
disagree about what a member is:

- It walks the BODIES and finds each one's path by provenance (a Cope leaves
  the member's Pipe in place; a Pattern names its source). A body that exists
  is a member, including one a mirror or pattern made; a curve whose tube was
  consumed is not.
- The cut length is MEASURED, not derived: every vertex of the body, with its
  edges sampled by deflection, is projected onto the path and the span of arc
  length it occupies is the metal (`MetalAlongPath`). A coped end is 8–9 mm
  shorter than the node-to-node centreline on a Ø19 brace into a Ø25.4 run,
  which is why `path_length_mm` and `cut_length_mm` are reported apart.
- Identical pieces — same stock, same bends, cut lengths within
  `kSameLength` (0.1 mm, finer than a saw and coarser than a boolean's
  residue) — collapse into one row with a `quantity`, reporting the LONGEST of
  the group so cutting to it can only leave a member long.

`tube_schedule {curve?}` answers with those rows: per member the quantity,
bodies, cut and path lengths, `coped_off_mm`, stock, and per bend the angle,
former, straight before and roll. A bend is `tight` against the declared
material and wall; with no rules it falls back to R < 1.5 × D and says so in
`tight_rule`.

## Checks

`check_tube` — every member against its own stock and the declared bender.
Kinds: `size_not_stocked`, `bend_below_minimum_radius`,
`radius_not_in_die_set` (naming the dies owned), `straight_below_clamp_minimum`,
`tail_below_gripper_minimum`, `wall_thinning_over_limit`. `not_checked` names
ovality and springback, which belong to the tooling and the machine. Refused
on a part with no tube rules.

`check_frame` — is the frame a truss, on the member graph rather than the
geometry (`src/core/tube/Frame.cpp`). Each member's centreline is clipped to
where its metal is, since members are swept long. Kinds:
`member_end_unattached`, `joint_on_a_bend`, `bay_not_triangulated` (a
four-member loop with no diagonal, flat to 5% of its own diagonal, remembered
by its four corners; past twelve, the rest are counted in `not_checked`).
`not_checked` also names stiffness (a beam model's question) and panels (a
floor or firewall braces a bay as surely as a diagonal, and nothing here can
see one), so an empty `violations` is not a claim that the frame is stiff.

Both return `{violations[]{kind, message, member, bend, actual, required},
members_checked, bends_checked, not_checked}`.

## Exports

- `export_cut_list {path, stock_length_mm?, kerf_mm?}` — CSV
  `member,quantity,material,size,cut_length_mm,bends,bodies`, columns following
  `export_bom`'s where they overlap. With a stock length it also packs the
  pieces first-fit decreasing, PER SIZE (a 19.05 member is not cut from a
  25.4 stick), taking the kerf between pieces, and reports the sticks and
  offcuts. There is no default stock length — it is a supplier's fact — and a
  member longer than the stock is refused.
- `export_bend_schedule {path}` — CSV, one row per BEND:
  `member,bend,distance_mm,angle_deg,roll_deg,former_radius_mm`, the order a
  bender is programmed in.
- `export_members {dir, overwrite?}` — one STEP per member, named for it and
  holding one product of the same name, ends already coped and mitred. It
  refuses to overwrite an existing file unless told to, and refuses two
  members with one name.

## Code

`src/core/tube/` — `Rules.{h,cpp}` (stock, rules, bend arithmetic), `Check`
(check_tube), `CutList` (members, measured cut length, stick packing),
`Frame` (check_frame). `src/core/features/CopeFeature.{h,cpp}`,
`PipeFeature`, `CurveFeature`. RPCs in `src/remote/RemoteTube.cpp`
(`set_tube_rules`, `get_tube_rules`, `check_tube`, `check_frame`,
`export_cut_list`, `export_bend_schedule`, `export_members`) and
`src/remote/RemoteInspection.cpp` (`tube_schedule`, `curve_points`).

GUI: the 3D Curve and Pipe toolbar tools make members. There is no GUI for
`Cope` or the tube rules; both are reached through `add_feature` and the RPCs.

## Tests

`tests/test_tube.cpp` (stock table, D/t rule, thinning, derived rules, die
set, validation, every `check_tube` kind), `tests/test_cope.cpp` (neighbour
survives, hollow neighbour cuts as solid, a cope that meets nothing, a gap on
a non-tube, a cope onto a bend, mitres and their angles), `tests/test_frame.cpp`, `tests/test_curve3d.cpp` (bent path), and the
tube section of `tests/headless_smoke.py` (schedule, cope, CSVs, member
STEPs, `curve_points`).

## Not built

- A weld is not a model object; a Cope saying two members meet is where one
  would hang.
- No beam elements: FEM meshes tet10 solids, so a frame's stiffness is not
  answered (`check_frame` says so).
- The exact contact sweep over a whole frame takes minutes; the chassis
  generator makes it opt-in (`--contact`).
- Ovality and springback are not modelled; a tube is not marked with its name
  (a flat blank's DXF can be, see `export_dxf {mark}`).
