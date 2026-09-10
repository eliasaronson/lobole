# Measure — from a distance readout to a measuring tool — design

> Status: **SHIPPED 2026-08-25**, except vertex picking (see "What shipped,
> and the one thing that did not"). Nine commits: the four standing wrong
> answers, then M1-M7. Every finding below is closed or explicitly deferred,
> and each phase carries the test that would have caught its bug.
>
> Three things the work found that this plan did not, all of them the same
> shape as the findings that prompted it — a plausible wrong number:
> the dimension was drawn at the MINIMUM points whatever distance_kind was
> selected (picture and number describing different point pairs); centre-to-
> centre read a point on the RIM, so two holes of different radii reported
> 102.6 mm where the spacing was 104.4; and a variable created from a
> measurement was formatted with thousands separators, which does not parse
> as an expression. Plus one pre-existing unrelated defect: `describe` listed
> `list_variables` twice, which had been failing remote_smoke since v64.
>
> Originally written 2026-08-25 as a **PROPOSAL**, from a
> screenshot in which the Measure tool, asked about two edges of one board,
> answered `Distance: 0.000 mm (nearest) / dx 0.000 dy 0.000 dz 0.000`. That
> answer is arithmetically correct and useless: the two edges meet at a
> corner, and the question was the angle between them.
>
> **Twelve findings, seven phases.** The findings were reproduced against a
> running instance, not read off the source; five of them are standing wrong
> answers rather than missing features, and three sit inside currently-passing
> tests.
>
> Supersedes the one-line backlog in [plan.md](plan.md) ("Deferred polish:
> point picking, angle-between-faces/edges, and the unified-selection (U1)
> integration + floating readout overlay") and the out-of-scope note in
> [../ui/dims-and-assembly-measure.md](../ui/dims-and-assembly-measure.md)
> ("farthest-distance and explicit parallel-face callouts"). Both of those
> read as polish; findings 3, 7, 8 and 9 say the tool is missing a floor,
> not a finish.

## Why

Measure shipped in two halves — the `measure` RPC (I1) and the GUI tool (C3)
— and each half was scoped as "resolve two element names, call
`BRepExtrema_DistShapeShape`". Everything since (assembly transforms, the
occurrence prefix, the orange line) has extended that one question. Nobody
has since asked what a person actually walks up to a model wanting to know.

The gap shows up the moment you use it on real geometry. Three natural picks
on one board, driven through the GUI on a scratch instance:

| picked | reported |
|---|---|
| two edges meeting at a corner (the user's screenshot) | `0.000 mm` |
| the top face and an edge lying on its boundary | `0.000 mm` |
| two adjacent faces of a box | `0.000 mm` |

Zero is the honest answer to "what is the minimum distance between these two
shapes" and the wrong answer to every question that made someone click. On a
single solid, most element pairs touch — so the tool's most common output is
`0.000`, and the number that would have been informative (the angle, the
parallel separation, the diameter of that bore) is not computed at all.

## Where we actually are

- **Core** — `src/core/Measure.{h,cpp}`. `Describe` (face area / edge length /
  circular-edge radius), `Distance` / `DistanceDetailed` (OCC minimum
  distance + the closest point pair, with an assembly overload that places
  each element by its occurrence transform). `DescribeFaceAt` /
  `DescribeEdgeAt` additionally classify a surface/curve and already compute
  a **cylinder/cone axis, a sphere/cylinder radius, a face centroid and
  normal, an edge midpoint and circle axis** — none of which `Describe`, and
  therefore neither the tool nor the RPC, ever returns.
- **RPC** — `measure` in `src/remote/RemoteInspection.cpp:1337`. `{a}` →
  area/length/radius; `{a,b}` → `{distance}` and nothing else.
- **GUI** — `MeasureState` (`src/ui/AppState.h:347`), the readout in
  `UpdateMeasureReadout` (`src/ui/AppActions.cpp:4356`), the tool window in
  `DrawMeasureToolWindow` (`src/ui/PreviewFeature.cpp:184`), the picked-element
  highlights and the orange line in `AppendSelectionPrimitives`
  (`src/ui/SelectionPipeline.cpp:598`).

## Twelve things found while looking

Each was reproduced, not inferred, except where noted.

**1 — There is no angle, anywhere.** Not in the core, not in the RPC, not in
the tool. This is the user's report and the headline of the work below.

**2 — `describe` says there is.** The `measure` row in `InspectionMethods`
(`RemoteInspection.cpp:146`) reads *"point/distance/angle/radius between
elements a and b"*. Two of those four do not exist. An agent that trusts
introspection — which is the entire point of `describe`, and the reason
`tests/test_conventions.cpp` scans for handlers without a row — will ask for
an angle and be told `unknown parameter 'kind'`. The MCP tool's own
description is honest; only the in-app introspection lies. This is the
"refuse, don't substitute" rule turned inside out: we refuse the call but
advertise the capability.

**3 — The distance line is never drawn on an assembly tab.** `UI.cpp:893`
builds the overlay primitives inside `if (state.document)`, and the `else`
branch (`UI.cpp:917`) calls `SetPrimitives({})`. `state.document` is null on
every assembly tab (`AppActions.cpp:1051`). So on assemblies — the context
assembly measure was *built for*, where "how far apart are these two parts"
is the whole question — the readout says `Distance: 427.814 mm` and the
viewport shows nothing at all. **Measured edges get no highlight there
either**, for the same reason; only faces do, because face highlighting goes
through a different (per-instance) path. Verified by screenshot.

The existing GUI test `measure/assembly_distance`
(`GuiTestsViewport.cpp:1151`) asserts `s_App->measure.hasDistanceLine` — the
*state* — and passes. Nothing asserts a primitive was emitted.

**4 — Measure state survives a tab switch.** `ResetActiveTabUiState`
(`AppActions.cpp:976`) clears the selection, slot picks, sketch edit, dim
tool and simulation mode. It does not clear `state.measure`. Switching from a
part tab to an assembly tab leaves the part's two picked faces listed in the
tool window with their distance, over an assembly that has never been
measured — refs resolved against a document that is no longer on screen.
Verified by screenshot.

**5 — The readout ignores the document's units.** `FormatElement` and
`UpdateMeasureReadout` hard-code `"%.3f mm"` and `"mm²"`. Units became a
document property in v64 (`units::FormatLength(mm, sys)`, and
`state.ActiveUnits()` is right there — the feature tree and the BOM already
use it). A part authored in inches measures in millimetres.

**6 — A cylindrical face reports its area and not its diameter.** Click a
bore in Measure and you get `Face area: 4712.390 mm²`. `DescribeFaceAt`
computed the 12.5 mm radius and threw it away; `list_elements` drops it from
its JSON too (`RemoteInspection.cpp:1395` emits kind/area/centroid/normal and
no radius). Hole diameter is the single most-asked measurement in mechanical
CAD and we answer it with a surface area.

The sharp edge on this one: `query_geometry` **does** return `radius` and
`axis` (`RemoteInspection.cpp:661` reads both off the same `DescribeFaceAt`).
So an agent can get a bore diameter and the person clicking the bore cannot.
The data is not missing; it is one RPC away from the two callers that need
it.

**7 — Measure is the one tool that never joined U1.** U1's premise is "one
typed, ordered selection set that every context reads and writes"
(`AppState.h:540`), and U7b made every tool seed its slots from it —
`SeedSlotsFromSelection` on entry (`PreviewFeature.cpp:39`). Measure keeps a
private `picked` list instead, and `ToggleMeasure` (`Commands.cpp:213`) opens
with `s.measure = MeasureState{}` — it *discards* the selection rather than
consuming it. Select two faces, press Ctrl+Shift+M, and you have to click
both again. Every other tool in the app works the other way round.

**8 — The RPC cannot measure across two parts, or two occurrences.**
`measure` resolves **one** body set and uses it for both `a` and `b`
(`RemoteInspection.cpp:1349`) — `part` picks which part, not which part per
ref. The GUI has measured across occurrences since the assembly-measure work,
and `Measure::DistanceDetailed` already has the six-argument
bodies-A/xf-A/bodies-B/xf-B overload that does it. So the assembly question
the GUI can ask, the scripting half cannot — against the standing
`clicking_and_scripting_agree` gate. The nearest available answer is
`interference`, which is not a name anyone reaches for when they want a
distance.

**9 — Bodies and occurrences cannot be measured at all, though the core
can.** The armed kinds are `Face|Edge` and nothing else
(`SelectionPipeline.cpp:276`). Meanwhile `Measure::ContactBetween` returns
the exact minimum separation between two rigidly placed body *sets* and
already backs `interference` and `contact_report`. "How far apart are these
two parts" is the most natural question in an assembly, the engine answers it
today, and the tool will not accept the pick.

**10 — The panel shows raw toponaming strings.** The tool window rows read
`Edge 1/face3;U0` and `Face 2/face3`. That is the internal stable name — the
thing that makes a measurement survive a regen, and exactly the thing a
person should never have to read. A row wants to say *Board — top face* or
*Board — 600 mm edge*.

**11 — Armed-but-inert inside a sketch.** `view.measure`'s enabled predicate
is `HasBody(s) || occurrences` (`Commands.cpp:356`), which is true while you
are editing a sketch on a part that has a body — so Ctrl+Shift+M arms measure
there. `HandleSketchViewport` consumes every viewport click first
(`ViewportInput.cpp:219`) and `AppendSelectionPrimitives` returns early in
sketch mode (`SelectionPipeline.cpp:559`), so nothing picks and nothing
draws. Combined with finding 4 the armed state then survives leaving the
sketch, where the next click on the model silently becomes a measure pick
instead of a selection.

Checked and cleared on the way past: the same predicate correctly *disables*
the command on a drawing tab, so the keybinding is not firing into a context
that has no geometry. That part is right.

**12 — Nothing in the test suite would catch most of the above.**
`tests/measure_smoke.py` measures every face of a cylinder in a loop —
including the cylindrical wall — and only ever asserts area and cap-to-cap
distance, so finding 6 sits inside a passing test. `measure/assembly_distance`
asserts `hasDistanceLine` (finding 3). No test measures across a tab switch
(finding 4), reads a unit (finding 5), or asserts the `describe` row against
the handler (finding 2). The gaps are not that the tests are weak; it is that
each one tests the thing the code was written to do.

### And the visualization, as it stands

A 2 px orange segment between OCC's closest point pair, and nothing else.

- **No value on it.** The number is in a panel in the opposite corner of the
  screen; the geometry is under the cursor. You read one, then look away to
  read the other.
- **It lands wherever the extrema solver stopped.** For two parallel 600 mm
  edges 45 mm apart, every point pair is 45 mm apart and OCC returns *one* of
  them — so the picture is a 45 mm stub at one far corner of a 600 mm board,
  which reads as a rendering artifact rather than as the thickness.
- **It is depth-tested** (`SketchRenderer.cpp:161` — the gizmo pipeline is
  the only on-top one), so any measurement whose line passes through material
  — plate thickness, bore to outer wall, the width of anything — is drawn
  *inside the solid* and invisible.
- **No arrowheads, no witness lines, no leader.** The sketch and drawing
  modules both draw proper dimensions already; the 3D viewport does not.
- **Measured faces highlight orange, measured edges blue** — both reuse the
  ordinary selection visuals, so a measured element looks exactly like a
  selected one, and the two kinds do not look like each other.
- **The prompt pill overlaps the tool window.** Visible in the user's
  screenshot and in every one taken for this document: "Measure: pick a face
  or edge (a second for distance) - Esc exits" is drawn over the tool
  window's first row.

## What mainstream CAD does that we do not

Consolidating SolidWorks' Measure, Fusion 360's Measure, and Onshape's
measure-in-the-status-bar. Not a wish list — the column on the right is what
each is *for*.

> **This table is the survey that opened the round (2026-08-25), kept as
> written.** Several of its ✗ rows are M1-M7 deliverables now — "copy the
> value / drive a variable from it" among them (`MeasuredValueForClipboard`,
> `CreateVariableFromMeasurement`). Read the phase list below for what
> shipped; do not read this table as current state.

| Capability | Us | Why it matters |
|---|---|---|
| **Angle** between two planes / lines / axes | ✗ | The user's report. Two faces that touch have no distance and a very useful angle. |
| **Vertex / point picking** | ✗ (no `Kind::Vertex` in the selection model at all) | Point-to-point is the fallback that answers a question no face pair can frame. Also: a vertex's XYZ. |
| **Snap to centre / midpoint / endpoint** while measuring | ✗ (exists for mates as `Kind::Connector`) | "Centre-to-centre between these two holes" — the second most common measurement after a diameter. |
| **Diameter / radius of a cylindrical face** | ✗ (area only) | See finding 6. |
| **Maximum distance**, and **centre-to-centre**, alongside minimum | ✗ | Minimum is one of three questions; the others are how you check clearance and hole spacing. |
| **Signed deltas** along X/Y/Z | ✗ (`std::abs` on each) | "Is it 40 mm *left* or *right*" is the difference between a part that fits and its mirror image. |
| **Face perimeter, bbox extents** | ✗ | Cut-list and stock questions. |
| **Total** length / area over >2 picks | ✗ ("keep two for a distance") | Weld length, coating area, cut length. |
| **Value drawn in the viewport** with witness lines and arrows | ✗ (bare line, no label) | See above. |
| **Hover preview before committing a pick** | ✗ | Onshape reads out whatever is under the cursor with no tool armed at all. |
| **Several measurements pinned at once** | ✗ (one pair, replaced) | Comparing three gaps means writing two of them down. |
| **Precision / unit control on the readout** | ✗ (fixed `%.3f mm`) | See finding 5. |
| **Copy the value / drive a variable from it** | ✗ | `plan.md` already lists measurement-driven variables as deferred. |
| **Component-to-component distance** (pick two parts, not two faces) | ✗ in the tool; the engine does it | Finding 9. The assembly question, asked the way people ask it. |
| **Select first, then measure** | ✗ (the tool clears the selection) | Finding 7. Every other tool here already does this. |
| **Readable names for what you picked** | ✗ (`1/face3;U0`) | Finding 10. |
| Wall-thickness probe, draft analysis, curvature | ✗ | Genuinely out of scope; noted so the omission is deliberate. |

Two we already have and they do not: **stable element names** (a measurement
names *which* face, and survives a regen) and **the same measurement over
RPC**, which is what lets an agent check its own work — though finding 8 says
the second one only half holds today.

### The shape of the gap

Very little of this is missing geometry. `DescribeFaceAt` / `DescribeEdgeAt`
already produce the normals, axes, radii, centroids and midpoints that an
angle, a diameter and a centre-to-centre need; `ContactBetween` already does
body-to-body distance; `DistanceDetailed` already takes per-side transforms.
Almost every row above is a *plumbing* gap — computed and dropped, or
computed and exposed to one caller out of three. That is what makes the work
below tractable, and it is also why it has stayed invisible: every individual
piece looks present from wherever you happen to be standing.

## Design

Seven phases. M1–M3 are the user's two asks plus the bugs those asks land on;
M4–M7 are the rest of the table. Nothing here changes the file format —
measurements are session state, not document state, in every phase. No golden
bumps, in any phase.

One rule holds across all seven, and it is the one the current tool breaks:
**a measurement never reports a number without reporting what the number is
of.** `0.000 mm` is true and unusable; `touching — 90.0° between the face
normals` is the same geometry, said. Every readout below carries its basis,
its units and its qualifier, or it is not shipped.

### M1 — Angle, and a readout that leads with the useful number

**Core.** One new concept: an element's *direction*, which is the only thing
an angle can be taken between.

```cpp
// The direction an element defines, when it defines one: a planar face's
// outward normal, a line edge's tangent, a cylinder/cone's axis, a circular
// edge's axis. nullopt for a sphere, a spline, a torus — refuse, don't
// substitute a plausible axis nobody asked for.
struct Direction {
  glm::dvec3 dir;
  bool oriented;   // false when the sign is arbitrary (a line has no "way")
  std::string from; // "plane normal" | "line" | "cylinder axis" | "circle axis"
};
std::optional<Direction> DirectionOf(const std::vector<Body>&,
                                     const ElementRef&);
```

`DescribeFaceAt` / `DescribeEdgeAt` already compute every one of these; this
is a resolve-and-classify wrapper, not new geometry.

```cpp
struct AngleResult {
  double degrees = 0.0;   // between the two directions
  double acute = 0.0;     // min(degrees, 180 - degrees)
  bool parallel = false;  // within tolerance — the angle is degenerate
  bool perpendicular = false;
  std::string basis;      // "between the face normals", "between the axes"
};
std::optional<AngleResult> Angle(const std::vector<Body>&, const ElementRef&,
                                 const ElementRef&);
```

**The convention, stated in the readout rather than assumed.** Two planar
faces are measured *between their outward normals*, so two adjacent faces of
a box read 90° and two opposite faces read 180° — which is what SolidWorks
does and what a draftsman means by "the angle between these faces". Two line
edges have no reliable direction sign, so they report the acute angle and say
so. `basis` carries that sentence into the readout; nothing is silently
picked.

**The readout leads with whichever number is not degenerate.** This is the
fix for the screenshot. Both quantities are always computed for a pair, and
the tool orders them:

- elements touch or intersect (`distance ≈ 0`) → **lead with the angle**,
  distance shown after as `touching`;
- directions are parallel → **lead with the distance**, and label it
  `parallel, 45.000 mm apart` rather than `(nearest)`, because for parallel
  elements the minimum distance *is* the separation and is the same
  everywhere;
- otherwise → distance first, then angle, both plainly.

An element pair where neither is defined (two splines, say) says so instead
of printing a zero.

**RPC.** `measure` grows `kind` — `"distance"` (default, unchanged),
`"angle"`, or `"both"` — and the `{a,b}` response gains `angle`,
`parallel`, `perpendicular`, `basis`, `point_a`, `point_b` and signed
`delta`. An unknown `kind` is refused naming the three that are accepted.
**Fix the `describe` row in the same commit** so it names exactly what the
handler does — that is finding 2, and it is the reason the row is wrong now.
The row's fourth claim, `point`, waits for M5 — take it out now and put it
back when the handler has it, rather than leaving it standing.

### M2 — Make the measurement visible on the geometry

A new overlay owner, `AppendMeasureOverlay`, replacing the two lines of
`AppendSelectionPrimitives` that draw the bare segment.

1. **The value goes on the model.** A pill at the midpoint of the dimension,
   drawn with the `WorldToScreen` + `GetForegroundDrawList` pattern CAM's
   depth label already uses (`CamTool.cpp:598`) — same rounded backing box,
   theme colors.
2. **Draw it on top.** Measurement overlays move to the gizmo pipeline
   (`depthTest=false`, already built — `SketchRenderer.cpp:236`) so a
   thickness or bore-to-wall measurement is not hidden inside the solid.
   Dashed where it passes through material, solid where it does not, if the
   dash costs nothing; solid-on-top otherwise.
3. **Put the dimension where it means something.** When the two elements are
   parallel, do not draw OCC's arbitrary extrema pair: project one element's
   centroid onto the other along the common normal and dimension *that*, with
   dashed witness lines back to each element. When they are not parallel the
   extrema pair is the right answer and stays.
4. **Arrowheads and witness lines**, the drafting default — the sketch
   dimension renderer's arrowhead code is the reference.
5. **Angles draw an arc**, not a line: from direction A to direction B about
   their intersection (or the shared vertex), value on the arc. The sketch
   angle-dimension renderer got exactly this treatment in
   [dims-and-assembly-measure.md](../ui/dims-and-assembly-measure.md) Part A
   item 2; reuse the layout, not the code (2D vs 3D).
6. **Measured ≠ selected.** Measured faces and edges get one colour, distinct
   from the selection blue and used for both kinds.
7. **Fix finding 3 while here**: the overlay owner is called on assembly tabs
   too. The clean form is to hoist the primitive build out of
   `if (state.document)` and let each contributor decide — most of them are
   genuinely part-only, measure and selection are not.
8. **Move the prompt pill** clear of the tool window (finding 6, cosmetic).

### M3 — Hover

Two readings of "a visualisation of the distances when I hover" — build both;
they are the same plumbing and they answer each other's gaps.

- **Hover a candidate in the viewport, see the measurement before you click.**
  With one element already picked, the element under the cursor is measured
  against it live and drawn dimmed — the full M2 dimension, value and all.
  Click commits it, moving on commits nothing. `state.pick.hoverFace` is
  already a stable `ElementRef`; the edge path needs the same `EdgeNameOf`
  lookup assembly measure added. With *nothing* picked, hovering reads out
  that element's own measure (area / length / diameter) in the same pill —
  Onshape's behaviour, and the cheapest possible "what is this".
- **Hover a row in the tool window, see which measurement it is.** Once M4
  keeps more than one, the rows need to point at the geometry: hovering a row
  brightens its dimension and dims the others.

Cost control: the hover measurement is an OCC extrema call per frame in the
worst case. Cache on (refA, refB, document version, occurrence transforms)
and compute only when the hovered element changes, not per frame.

### M4 — More than one measurement, and more than one question

- **Pin.** The current pair becomes a *measurement*; "Pin" moves it to a list
  and clears the picks for the next one. Pinned measurements draw until
  dismissed, and drop themselves on a document edit that breaks a ref (the
  `compare_geometry` overlay's lifecycle rule — same drop-on-edit,
  drop-on-tab-switch discipline).
- **Distance qualifier.** Minimum (default) / maximum / centre-to-centre,
  chosen per measurement. Maximum is `BRepExtrema_DistShapeShape` on the
  complements — cheap; centre-to-centre is the two centroids, which
  `DescribeFaceAt`/`DescribeEdgeAt` already return.
- **>2 picks sum.** Three edges report total length, three faces total area,
  a mix refuses and says why. This replaces `"3 elements selected - keep two
  for a distance"`, which is a refusal with no reason.
- **Fix finding 4**: `ResetActiveTabUiState` clears `state.measure`,
  pinned measurements included. That also closes finding 11's tail — an
  armed-in-a-sketch measure no longer survives to eat the next click.

### M4b — Measure joins the selection model

Finding 7, and it is a deletion more than an addition. `MeasureState::picked`
duplicates a `SelectionItem` list that already carries kind, element ref,
occurrence id and pick order, and it exists in parallel with the set that U1
declares canonical.

- `ToggleMeasure` **seeds from `state.selection`** instead of clearing it —
  the `SeedSlotsFromSelection` behaviour every other tool has had since U7b.
  Select two faces, hit Ctrl+Shift+M, read the answer.
- The measured set becomes a view over `state.selection` filtered to
  Face/Edge (and, after M5, Vertex/Body/Occurrence), so a measured element
  highlights, deselects, and is named by the same code as everything else —
  which is also how it stops needing its own highlight branch in
  `SyncSelectionHighlights` and its own edge branch in
  `AppendSelectionPrimitives`.
- Keep exactly one thing that is genuinely measure's own: pick **order**
  already matters to the selection set ("ordered: pick order matters to
  tools"), so `a` and `b` fall out of it.

Sequenced after M4 deliberately: pinning gives the tool a place to keep a
measurement that is *not* the live selection, which is what makes collapsing
the live picks into the selection set safe rather than lossy.

### M4c — Say what was picked

Finding 10. One helper — "how a person names this element" — shared by the
tool window rows, the M2 viewport pill and the M3 hover readout:

    Board — top face          (plane, 95 000 mm²)
    Board — 600 mm edge       (line)
    Bracket — Ø25 bore        (cylinder)

Body name, then what the element is, derived from the classification
`DescribeFaceAt`/`DescribeEdgeAt` already return. The stable name
(`1/face3;U0`) stays — as the tooltip, and in the RPC, where it is the thing
that matters.

### M5 — The elements we cannot pick

- **Bodies and occurrences** (finding 9) — the cheap half, and it goes first
  within this phase. Add `Body` and `Occurrence` to measure's armed kinds and
  route the pair to `Measure::ContactBetween`, which already returns exact
  minimum separation between two placed body sets and already backs
  `interference`. The readout says `clear, 12.400 mm apart` /
  `touching` / `overlapping by 340 mm³` — `ContactBetween` returns the volume
  too, so a clash reports as a clash instead of as distance 0. This is the
  assembly question stated the way people ask it, and it is a day's work.
- **`SelectionItem::Kind::Vertex`**, and a picker for it. This is the largest
  single piece of work in the document and the reason it is fifth: it touches
  the selection model, the pipeline, the filter masks and the renderer, and
  everything above is useful without it. Payoff: point-to-point measurement,
  a vertex's XYZ readout, and a foundation the mate tool and sketch snapping
  can both use.
- **Snap targets while measuring**: hole centre, arc centre, edge midpoint,
  face centroid — the `Kind::Connector` snap the mate tool already does,
  offered as measurable points. Centre-to-centre between two holes is the
  measurement this unlocks and the one people ask for by name.
- **Datum planes and axes** as measurable elements (distance from a face to
  the Front plane is a real question we cannot currently ask). `DirectionOf`
  from M1 extends to them without change — a datum plane is a normal.

### M6 — The readout itself

- **Units and precision from the document** (finding 5):
  `units::FormatLength(mm, state.ActiveUnits())` everywhere in the readout,
  `units::FormatAngle` for angles, and significant figures rather than three
  fixed decimals — `units::FormatNumber` already implements the rule.
- **Diameter, radius and perimeter on single elements** (finding 6):
  `Measure::Element` grows the fields `DescribeFaceAt` already computes; the
  RPC and `list_elements` return them.
- **Copy value**, and — the `plan.md` I4 item this unblocks — **create a
  variable from a measurement**, which is the point at which measuring stops
  being read-only.

### M7 — The scripting half catches up

Finding 8, and the parity gate it sits against. Today the GUI can measure
across two occurrences and the RPC cannot address two parts at once.

- **Per-ref part addressing.** `a` and `b` each accept an optional `part`
  (name or stable id) and, on an assembly, an `occurrence` (id or path — the
  `"Gangway/Board 19"` form `interference` already accepts). The top-level
  `part` stays as the default for both, so every existing call is unchanged.
  The implementation is the six-argument `Measure::DistanceDetailed` overload
  the GUI has been using since assembly measure shipped; nothing new in core.
- **Body and occurrence refs** as measurable `a`/`b` — the RPC face of M5's
  first bullet, so `measure` answers the "how far apart are these two parts"
  question under the name people look for, rather than only under
  `interference`.
- **Everything the GUI computes comes back**: `angle`, `basis`, signed
  `delta`, `point_a`/`point_b`, the qualifier, and `radius`/`diameter` on a
  cylindrical face — the M1 and M6 additions, exposed rather than re-derived.
- **`list_elements` stops dropping `radius` and `axis`** for faces
  (finding 6): it reads them from the same `DescribeFaceAt` call it already
  makes, and `query_geometry` proves the JSON shape works.
- **The parity assertion is the deliverable**, not a side effect: extend
  `parity/clicking_and_scripting_agree` so a measurement taken by clicking
  two occurrence faces and the same measurement taken over RPC produce the
  same numbers. That gate is what stops the two halves drifting again.

## Small fixes that need no phase

Each is a few lines, none depends on this design being signed off, and each
can ride along with whichever phase touches its file or land on its own:

| | Fix | File |
|---|---|---|
| 2 | The `describe` row names only what the handler does | `RemoteInspection.cpp:146` |
| 4 | `ResetActiveTabUiState` clears `state.measure` | `AppActions.cpp:976` |
| 5 | Readout uses `units::FormatLength(mm, state.ActiveUnits())` | `AppActions.cpp:4356` |
| 6 | `Describe` and `list_elements` keep the face radius | `Measure.cpp:663`, `RemoteInspection.cpp:1395` |
| — | The prompt pill stops overlapping the tool window | `Overlays.cpp` |

## Tests

- **Core** (`tests/test_measure.cpp`, headless): angle between two adjacent
  box faces = 90°; between opposite faces = 180° with `parallel` set; between
  two perpendicular edges of a box = 90°; between a cylinder axis and a
  planar face; `DirectionOf` returns nullopt for a sphere and for a spline
  edge. Signed deltas keep their sign under a swap of `a` and `b`. Maximum
  distance across a box's diagonal.
- **RPC** (`tests/measure_smoke.py`): `kind:"angle"` on the pair from the
  screenshot returns 90; an unknown `kind` is refused naming the three
  accepted; the `describe` row for `measure` names only what the handler
  implements — assert this **from** `describe`, so the row cannot drift again
  (finding 2 is exactly that drift).
- **GUI** (`gui-tests-xvfb.sh`): the assembly-tab regression from finding 3
  gets the assertion the existing test lacks — after measuring across two
  occurrences, assert the primitive buffer is **non-empty**, not just that
  `hasDistanceLine` is set. Switching tabs clears `measure.picked`
  (finding 4). Hovering a second element with one picked produces a readout
  without changing `picked`.
- **Parity** (`parity/clicking_and_scripting_agree`): the same measurement
  taken by clicking and taken over RPC agrees, across occurrences as well as
  within a part. This is M7's deliverable, and the reason finding 8 could
  exist is that the gate does not currently cover measure at all.
- **Visual**: the dimension pill, arrowheads, the angle arc and the
  through-material on-top draw are screenshot review on dev-watch — same as
  the sketch dimension work.

### The test lesson from finding 12

Three of the twelve findings sit inside currently-passing tests: the
cylindrical wall is measured in a loop that only ever asserts area
(finding 6), and the assembly line test asserts the state flag rather than
the primitive (finding 3). Both tests are correct about the thing their code
was written to do. Two habits close that class:

- **Assert the output, not the intent.** `hasDistanceLine` is a variable;
  a non-empty primitive buffer is a picture. Prefer the second whenever the
  claim is that something is visible.
- **Assert introspection against the handler.** The `describe` row and the
  accepted-parameter list should be read *from the running app* and compared,
  so a row cannot promise what no branch implements. `test_conventions.cpp`
  already does the structural half (no handler without a row); this is the
  semantic half.

## What shipped, and the one thing that did not

Everything above, including **M5's vertex picking** — which shipped
2026-08-25 as a CORNER rather than a vertex, for the reason below.

What that phase was FOR is mostly delivered by other means: point-to-point
between holes is `distance_kind: centres` on two circular edges or two
bores, which now uses the true centre; and the snap targets it listed (hole
centre, arc centre, face centroid) are exactly what CentreOf computes,
derived from a named element so they inherit its stable identity.

### The blocker was mis-stated, in two workstreams at once

The reason recorded here — a vertex has no ElementMap entry, so it has no
stable name, so naming vertices is a toponaming change — is true of a RAW
vertex and irrelevant to the thing anyone wants. A corner is where two
NAMED edges of a face meet. Both halves are stable, so the pair is.

The assembly side reached the same wall from the other direction a month
earlier and wrote down the same reason: A2.5 phase 4b closes with "Vertex/
corner snapping remains deferred because `ElementMap` does not yet provide
persistent vertex names", while the mechanism it shipped in the same phase
— `ConnectorAnchor::snapEdge`, whose origin is "that edge's centre, else its
midpoint" — is already a stable derived point from a named element. The
answer was sitting inside the thing that declared it impossible.

A kernel spike (2026-08-25) settled the two facts a design needs: on a box,
every face offers 4 unambiguous corners with all 4 bounding edges named; on
a cylinder's wall, one edge pair shares TWO vertices, so the ambiguous case
is real from the first curved face and must be refused rather than guessed.

Shipped as [../assemblies/a2_5-design.md](../assemblies/a2_5-design.md)
phase 4c, since that is where a corner is first USED, plus a
`MeasureState::Element::Kind::Corner` carrying its two refs. One corner
reports a COORDINATE — the one thing no face, edge or body can say, and the
whole reason point picking was ever wanted — and two report the distance
between them exactly, with every distance kind agreeing because a point has
no near end and far end to disagree over.

A raw vertex is still not nameable and still not offered. Nothing was lost
by that: what people want a vertex for is the corner.

Also not done, and recorded rather than smuggled in: `measure` and
`interference` have near-twin occurrence resolution (id / name / "Unit/Part"
path → world-placed bodies). They should be one function. Interference's
version shares a prefetched body map with the contact sweep, so merging them
is its own change with its own test run.

## Order & effort

**M1 → M2 → M3** is the spine and answers the report. Then **M4 → M4b → M4c**
as one session, **M7** whenever the parity gate is being touched, and **M5**
last and on its own.

| Phase | What it buys | Rough size |
|---|---|---|
| **M1** angle + informative readout | the reported bug | small, mostly core |
| **M2** dimension on the geometry | the tool becomes a tool | medium, viewport work |
| **M3** hover | measuring without committing | small once M2 exists |
| **M4** pin + qualifiers + sums | more than one question at a time | medium |
| **M4b** join the selection model | select-first; deletes parallel state | small, net negative lines |
| **M4c** readable element names | stop showing `1/face3;U0` | small |
| **M5** bodies/occurrences, then vertices | the picks we cannot make | bodies: a day. vertices: its own session |
| **M6** units, diameters, copy/variable | the readout stops lying about units | small |
| **M7** RPC parity | an agent can ask what a person can | medium |

- **M1** is the user's actual report and is mostly core: `DirectionOf` and
  `Angle` are a hundred lines over data `DescribeFaceAt`/`DescribeEdgeAt`
  already produce, and the lead-with-the-informative-number rule is what
  turns `0.000 mm` into `90.0°`. It also carries the `describe` fix, which
  should not wait behind anything.
- **M2** is where the tool stops being a panel and starts being a
  measurement, and it sweeps up the assembly-tab bug (finding 3) on the way,
  because that bug lives in the code M2 rewrites.
- **M3** is small once M2 exists — the drawing is the hard part; hover just
  supplies a second ref.
- **M4b before M5**, always: adding three new measurable kinds to a private
  pick list that duplicates the selection set means writing the duplication
  three more times. Collapse it first, then extend one set.
- **M5's first bullet (bodies and occurrences) is the cheapest large win in
  the document** — `ContactBetween` already exists, and it answers the
  assembly question nobody can currently ask by clicking. It does not need
  vertex picking and should not wait behind it.
- **M5's vertex picking** is the one piece that deserves its own session and
  its own review; it changes the selection model, not the measure tool.

The five entries in **Small fixes that need no phase** are each a few lines
and none needs this design signed off first — findings 2, 4, 5 and 6 in
particular are standing wrong answers, not missing features, and are worth
landing before any of the phases start.
