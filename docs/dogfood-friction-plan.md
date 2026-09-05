# What two engine rounds turned up, and what to do about it

Two dogfooding rounds built the same engine twice — once from primitives
(docs/modeling/single-cylinder-dogfood.md) and once from sketches
(docs/modeling/aircooled-dogfood.md). Between them they produced eight fixes
that already shipped and this list, which did not.

Ordered by what it costs to leave alone. Every item carries the evidence it
came from; where a fix is speculative it says so.

---

## F1 — nothing catches a shape that is CONSISTENT but silly

**The evidence.** The air-cooled barrel's four bolt bosses hung in mid-air:
a d=38 fin relief at r=65 reaches r=46, the d=30 boss only r=50, so the relief
ate four millimetres of the wall the boss had to land on. Four pillars
dangling off the deck flange.

Everything passed. It is ONE solid body — the flange joins the bosses at the
top. The motion study was clean over 49 samples. All four bolts fitted and
sized correctly. `check()` was green. None of those asks whether a shape makes
sense; they ask whether it is *consistent*, and it was. A person opening the
part found it in about two seconds.

There is no general "is this sensible" checker and this plan does not propose
one. Three specific things are available:

**(a) `section` should report REGIONS, not just a total area.** Measured on the
broken part:

    z = 222 (in the flange)   area 13006.95
    z = 200 (in the boss)     area  3293.76
    z = 120 (below the boss)  area  2586.90

The difference between 200 and 120 is 706.9 mm², which is exactly π·15² — the
boss disc, sitting as its own island in a section that the API reports as a
single number. Returning the closed regions (count, area, centroid) makes
"this feature is disjoint from the rest at its own mid-height" a question
somebody can ask cheaply. It is advisory, not a refusal: real parts have
islands in section. OCC already builds the section wires, so this is reporting
what is thrown away. **Small, and the highest-value item here.**

**(b) The literals lint that G0 deferred.** The boss floated because
`BOLT_R=65`, `WALL_R=52`, relief `d=38` and boss `d=30` were four independent
numbers with an unstated relationship. Had the boss diameter been derived —
`2 * (BOLT_R - WALL_R + 3)` — it could not have floated, and editing the wall
would have carried it. docs/agent-scale-plan.md deliberately deferred this
("a lint that counts literals where an expression could be is a later, cheaper
idea than a rule the SDK follows"). Two rounds later it has its evidence.
Scope it to what is checkable: a feature dimension that is a bare number while
a variable of the same value exists in scope is worth naming. **Medium, and
easy to make annoying — it must advise, never refuse.**

**(c) Make LOOKING routine, since looking is what worked.** A
`part.render(path)` on the SDK that fits and screenshots one part, and a line
in the dogfooding loop that says to use it. Cheap, and honest about the fact
that the check that caught this was a human eye.

---

## F2 — the profile vocabulary is thin, and one of its shapes surprises you

Building parts from sketches instead of primitives is the direction this
project wants. Two gaps found doing it:

**No `arc()`.** `SketchBuilder` has point, line, circle, rect, centered_rect,
polyline, polygon and slot. `slot` emits `arc` entities internally, so the
entity type exists and only the helper is missing — and a drawn profile with
a radius on it is the normal case, not an exotic one. Every rounded corner in
the air-cooled engine is a `slot` or a polygon because of this. **Small.**

**A slot's end cap is half its width past the centre it ends on**, and nothing
says so until it collides. A crank web drawn as a slot from journal to pin,
90 wide round a pin 43 up, reached z=85 — the barrel's underside is at 82, and
the motion study called `barrel x crank` at seven of 49 samples. The docstring
should say it, and `SketchBuilder` returning the profile's bounding box would
let a caller check before extruding. **Small.**

**Overlapping loops in one sketch are refused** — correctly, "profile loops
overlap or nest inconsistently", because unioning them would guess which loop
was meant to be a hole. But the workaround is mechanical: one extrude per
shape, each `mode="add"`. An SDK helper that takes a list of shapes and does
that would remove the boilerplate without weakening the refusal. **Small.**

---

## F3 — refusals that are not refusals

**`modify_feature` leaks a raw exception.** Sending a partial feature —
`{"depth": 20.0}` — comes back as:

    [json.exception.out_of_range.403] key 'type' not found

That is an nlohmann internal, not a refusal. Everything else in this surface
names what it accepts. It should say that a feature must carry its `type` and
that `modify_feature` replaces the whole spec, or accept a partial merge.
**Small, and it is the house rule.**

**The target parameter has three names.** Across 230 RPCs: 37 take `id`, 18
take `<thing>_id`, 7 take `index`. `modify_feature` takes `index` ONLY, so an
id in hand has to be turned into a position first; `select_feature` takes
`feature_id`; most things take `id`. Each is defensible alone and the set is
not learnable. This is not worth a breaking rename, but `modify_feature`
accepting `id` alongside `index` is worth it — that is the one that costs a
round trip. **Small.**

---

## F4 — a drawing view that can EXCLUDE occurrences

Already argued in docs/known-bugs.md and the only fix left worth building for
drawings. Exact HLR costs ~2.2 s per pair against a swept surface: the engine
is 47 s a view, and 0.0 s with its two valve springs hidden. Seventeen of its
nineteen bodies project exactly and instantly.

A view that names occurrences to leave out would give a real, dimensionable
drawing of this engine in near-zero time. Everything else has been measured
and does not pay — sweep tolerances, `MakePipe`, capping the spine's degree
(worse), and screen-rectangle pair skipping (correct, saves nothing). **Medium,
and it needs a format bump for the view's exclusion list.**

---

## F5 — the benchmark cannot measure any of this

Ten tasks, 3 arms, 3 reps: 90 runs and a $180 ceiling, and the tasks are
prose-describable parts with analytically computable volumes. Nothing in
either engine round — not the batch RPC, the SDK, datum frames, motion
studies, mesh collision, tabless reads or the drawing modes — has a benchmark
number behind it, and benchmarks/REPORT.md says so plainly.

The honest position is that it is not worth running until it measures
something this work changes. What would: a task that builds a MECHANISM and is
graded on whether it moves (T4/T5 are a start and have never been swept), and
a task graded on RELATIONSHIPS an oracle can check without a reference number
— these faces are parallel, this boss reaches this wall, this wall is
uniformly thick. The last of those is F1(b) wearing a different hat.

**Not in this plan**, because it is a workstream rather than an item: it needs
its own design, and spending $180 on the current suite first would buy a
number nobody should act on.

---

## Papercuts worth fixing while nearby

Each cost real time this round and each is a few lines:

* **`_MOVES_TAB` is hand-maintained** in the SDK — the list of RPCs that
  activate a different tab. A new tab-creating RPC will not be in it, and the
  symptom is the one this round already hit twice: a stale cached tab and a
  refusal about the wrong document kind. Derive it from `describe`, or have
  the server report which methods move the tab.
* **`list_drawing_geometry` returns `geometry`**, not `curves`; **`screenshot`
  takes `fit/path/view`**, not width/height. Both cost a guess-and-retry.
  A `describe` that carried RESULT keys as well as parameters would end that
  whole class.
* **CLAUDE.md says the human's instance is on 8799.** It is on 9090. The
  instance registry is the source of truth and the doc should point at it
  rather than repeat a number.
* **No `hide_body` RPC and no read-back for the selection**, so the per-tab UI
  state that W2 exists to protect cannot be asserted in a test — the gate
  checks the mechanism (no tab switch) instead of the consequence.
