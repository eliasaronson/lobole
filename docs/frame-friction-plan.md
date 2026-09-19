# The friction a chassis found

Four things the Formula Student chassis round cost time on
([chassis-round-plan.md](chassis-round-plan.md)). Each is small; together
they are most of what made building a 47-member frame slower than it had to
be, and one of them is a manufacturing rule the app already half-knows.

Beam elements are deliberately NOT here. A space frame wants them and this
build has only tet10 — one 2.6 m member meshes to 33,178 elements because
the 1.65 mm WALL sets the size, and a multi-body part is solved as
independent bodies ("body 'B' has loads but no fixed support"), so 47 welded
members are 47 unrestrained lumps. That is a workstream, not a fix, and it
gets its own plan.

## F1 — `describe` says what a method RETURNS

Every refusal this project makes is excellent: a wrong key is named, a
missing field is named, an unknown enum lists the accepted set. The
PARAMETER contract is in good shape and `rpc_params_gate` keeps it true.

The RESULT contract does not exist. `MethodDoc` is `{name, doc, params,
flags}` — there is nowhere to say what comes back, so the shape lives in
prose when it lives anywhere. Three stumbles in one afternoon, all of them
surfacing as a Python `KeyError` rather than as a refusal:

* `mass_properties` — the row says "per body and total", so the total looks
  like `r["mass_g"]`; it is `r["total"]["mass_g"]`.
* `list_elements` — nests under `bodies` even when the caller asked for one
  body by index.
* `list_parts` — returns parts, assemblies AND drawings, so iterating "the
  parts" needs a `type` filter, and getting it wrong answers "the active
  document is a drawing, which has no feature list" from somewhere else
  entirely.

A refusal teaches; a `KeyError` three calls later does not.

`MethodDoc` gains `returns`: the top-level keys of the response, in the same
comma-separated shape `params` uses, with the nesting shown where it matters
(`total{mass_g,volume,center_of_mass}`). `describe` reports it beside
`params`.

Required — and gated — for every `list_*`, `get_*` and `check_*` method, 40
of the 263 rows. That family is exactly the one an agent PARSES rather than
merely calls, and it is a rule a scanner can enforce rather than a
convention that rots. The other 223 may carry `returns` and mostly will not;
a mutating verb's answer is its `regen` summary and the rows say so already.

Files: `src/remote/RemoteHelpers.h` (the field), every `Remote*.cpp` with a
row in that family, `src/remote/RemoteDispatch.cpp` (the describe endpoint),
`tests/rpc_returns_gate.py` + `tools/rpc_returns_scan.py` beside the params
pair.

Verified by: the gate failing on a row with no `returns`, and — where the
handler builds its object literally — on a row naming a key the handler does
not emit.

## F2 — a cope onto a BEND says which it is

Putting the nose ring where the lower rail kinks made `Cope` refuse, which
was right, with a message that was wrong: it blamed tangency and said to
open the gap. The real cause is that the cut lands on the neighbour's BEND,
where the tube is a torus, thinned by the bending, with no straight to fit a
cope against. A joint does not belong on a bend, and that is a rule a shop
would state rather than a kernel quirk to work around.

The feature already knows enough to say so: it has the neighbour's path and
the member's end, so it can ask how far along that path the cut lands and
whether that falls inside an arc. When it does, the refusal names it and
says to move the joint clear of the bend — naming the tangent points, which
are where "clear" starts.

Files: `src/core/features/CopeFeature.cpp`, `tests/test_cope.cpp`.

## F3 — `curve_points`: where the metal actually runs

A bent path's `points` are the CORNERS. The metal turns short of them — with
a 152 mm former on a right angle, 63 mm short — so anything asking "does this
member pass through that point" has to reconstruct the arcs. The chassis
generator does exactly that, in Python, duplicating what `CurveFeature`
already computed and can drift from.

`curve_points {curve, deflection?}` returns the evaluated polyline of a
Curve3D — the one the feature built, not a re-derivation — with the arc
length at each point, so a caller can ask where along a member something
sits without owning a second copy of the geometry.

Files: `src/remote/RemoteInspection.cpp` (+ its `MethodDoc` row, with
`returns`), `tests/headless_smoke.py`.

## F4 — `check_frame`

Nothing told me the frame was under-braced. The front bay had no diagonal at
all over 570 mm, and both hoops carried 192 mm of unbraced crown; finding
both took Python written by hand. Every check this app has is per-PROCESS —
`check_sheet_metal`, `check_tube`, `check_fasteners`, `check_fits` — and
none is structural.

`check_frame(part)` → violations in the shape the others use:

```
member_end_unattached     a member welded to nothing at one end
joint_on_a_bend           a joint landing inside a neighbour's bend arc
bay_not_triangulated      four members forming a closed loop with no
                          diagonal across it, and no fifth member inside it
```

The third is the valuable one and the one with a caveat, so it carries it:
a bay can equally be braced by a PANEL, and this build has no way to know a
panel spans it. That goes in `not_checked`, the way `check_tube` says it
cannot judge ovality — an empty `violations` must not read as "this frame is
stiff", which is a question only a beam model answers.

It is a graph over the members, not geometry: nodes are joints (member ends
clustered to where they meet), edges are members, and a bay is a 4-cycle
with no chord. `tube::Members` already supplies the members; `Cope` already
records which meet which.

Files: `src/core/tube/Frame.{h,cpp}`, `src/remote/RemoteTube.cpp`,
`tests/test_frame.cpp`.

Verified by: the chassis as it was before triangulation — the front bay's
missing diagonal and the nose ring on the rail's bend are both in git, and
both must be found.

## Order

F1 first: it is mechanical, it is the one that cost the most time, and every
row the later stages add should carry `returns` from birth rather than be
retrofitted. Then F2 (small, and it makes F4's third violation kind
meaningful), F3, F4.

## As built

All four shipped. Headless suite green; the round's own numbers:

**F1** — `MethodDoc` gained a `returns` column, required and gated for 40
methods: every `list_*`, `get_*` and `check_*`, plus the three readers named
for their noun instead of a verb (`mass_properties`, `contact_report`,
`tube_schedule`). `tests/rpc_returns_gate.py` keeps it honest in one
direction — a key the row names must appear in the handler — because a
response nests and a scanner reading `{"name",` cannot tell a top-level key
from a leaf. It earned its keep while being written, catching prose I had put
in three key fields ("or null", "on a PART tab a row is..."), which would
have shipped as promised keys that do not exist.

Still to do, deliberately not guessed at: `interference`, `measure`,
`query_geometry`, `stack_up` and `compare_geometry` are reads too. Their
shapes want reading from the handler; a contract written from the prose is
the thing this is against.

**F2** — the cope's refusal tells the two failures apart. Reproduced on the
rail that caused it: on the kink it refuses by name, 30 mm past the kink the
same joint cuts, and on a straight it cuts. All three are in the test,
because the first version of that test used a sharper bend and cut cleanly —
a diagnosis that cannot reproduce the case it explains is a guess.

**F3** — `curve_points` returns the polyline the feature built, sampled to a
deflection, with the arc length of each point. On a 152.4 mm former it shows
the metal missing the corner by 63.1 mm, which is `R*(sqrt(2)-1)` and the
distance a brace was once attached across.

**F4** — `check_frame`, measured against the chassis before and after it was
triangulated: **21 untriangulated bays and 6 joints on bends before, 16 and
19 after**, the bays falling while the member count rose from 32 to 47.

Two decisions inside it. A bay is a FACE, flat to 5% of its own diagonal: a
skew four-cycle is a mechanism too in pin-jointed theory, but as a finding it
is noise, and the first version reported forty of them on a frame somebody
had just triangulated. And a bay is remembered by its four corners, because
the search reaches each one from either diagonal and double-counted every
one.

## What it found that is still open

`check_frame` on the chassis reported **19 joints landing on a bend**: the
lower rails and the upper side members kinked exactly at the hoop stations,
which are the busiest nodes in the frame. The copes all cut — the bends reach
only 2.3 to 3.7 mm either side — and it was still the wrong place for them: a
node is where five or six members converge, and a bend is the one stretch of
a tube with no straight to fit them against, made of a torus and thinned by
the bending.

**Fixed in the chassis, and it is a rule worth carrying: a bend belongs where
nothing is welded.** The taper now happens BETWEEN stations — two points on
the rails, one on the side members, chosen because nothing lands at them —
and every member runs straight through every station where something does.
The hoop stations' widths are what the taper line is drawn through, so the
line and the stations cannot disagree.

**19 joints on a bend became 0**, and the untriangulated bays fell from 16 to
9 with it, because moving the taper changed which faces close. The nine that
remain are mostly floor and side faces that a panel would close, which the
check says outright it cannot see.

The 16 remaining bays are mostly faces a floor or a firewall would close, and
the check says outright that it cannot see a panel.
