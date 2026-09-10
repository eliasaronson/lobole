# CAM for someone who has never done CAM

> Status: **BUILT** (2026-08-21), G1–G4. From: *"make the GUI more intuitive —
> selecting the top face visually, immediately understandable for someone who
> has never done CAM, as few clicks, settings and writes as possible."*
>
> The engine ([plan.md](plan.md)) was built and tested first. This was only
> about how a person reaches it. What follows is the design as written, with
> an [as-built section](#as-built) at the end recording what changed and what
> the first screenshots caught.

## What is wrong with the panel today

It is a parameter editor: a setup combo, a tool table, and per-operation rows
of `top / bottom / stepdown` drag fields. Everything it asks for is a number
about geometry the app can already see — and it asked the user for `2.00 →
-0.00`, which produced no toolpath and a refusal reading *"check its depths
against the model"*. That is the whole problem in one line: **the app knew the
answer and asked anyway.**

A CAM beginner does not have depths. They have a part, and an intent: *clear
that pocket, cut this out, face the top.*

## The shape: verbs, not fields

A toolbar button opens **one Tool-window session** (the U7/U8 pattern the
Section tool uses), and the session offers verbs. Each verb arms a face pick;
one click on the model completes it.

```
Zero:  [ Pick top face ]        ← Z zero, stock top, work origin
────────────────────────────
  [ Clear a pocket ]  → click the pocket floor
  [ Cut it out ]      → click the outline, or nothing = the whole part
  [ Break an edge ]   → click the wall whose top edge to round/chamfer
  [ Face the top ]    → no pick
```

The CAM dock tab stays, holding the **list** of setups and operations — a
list belongs in a panel. Editing a number stays possible; it stops being the
way in.

**Two clicks to a pocket, nothing typed.** Picking the floor gives the
operation its boundary (the face's outer wire), its `bottomZ` (the face's own
Z), its `topZ` (the stock top), and its stepdown (from the tool). The
`2 → -0` failure cannot be expressed.

### Why a tool session and not a mode

Simulation is a mode because it takes the viewport over — bodies hidden, a
field overlay, BC picking that means something other than selection. CAM
needs none of that: the backplot coexists with the model, and a picked face
means "that face".

And modes have a **known failure class in this codebase** — U11 spent a round
on simulation mode exiting when it should not (`File > New` left it), because
transitions were scattered across call sites. A tool session has a far
smaller surface, and the Section tool has needed no exit fixes. Choosing the
session *is* choosing to avoid that class.

If CAM ever earns a mode (stock rendering, machine limits, toolpath
playback), the rule to carry from U11 is: **one `EnterCam`/`ExitCam` pair, the
mode owned by the active part tab, and a test per transition** — new project,
open project, switch tab, delete the setup, undo.

## The visualisation ladder

For a beginner the picture *is* the interface. Ranked by confusion removed:

1. **The tool, to scale, at the moment of the pick.** A translucent cylinder
   of the real diameter in the pocket corner. It answers the question a
   first-timer does not know to ask — *will my cutter fit, and what radius
   will it leave?* Highest value of anything here.
2. **The material that will disappear.** Ghost the volume removed: the pocket
   void from stock top down to the picked face. Confirms the pick better than
   any number.
3. **Hover preview before the click** — highlight the region that *would* be
   cleared. The pick pipeline already hovers faces; this is wiring.
4. **Depth as a label on the part** ("3.0 mm"), so the derived number is
   visible without reading a field.
5. **Stock as a translucent box**, because where Z zero is and how much
   material sits above the part are otherwise invisible — and Z zero is the
   expensive mistake.
6. **Leftover painted on the part** after Generate. The verifier already
   computes a height-map difference, so tinting what the tool cannot reach
   (inside corners) is nearly free, and most hobby CAM makes you infer it
   from a backplot.

**The discipline that keeps this from becoming noise:** show only the
consequence of the decision being made, and drop it when the decision is
made. Hover → candidate region. Picked → removed volume + tool + depth label.
Generated → backplot + leftover. Session closed → everything but the backplot
goes away.

Most of it is assembly, not new rendering: ghosts exist (in-context edit,
topo overlay), dimension labels exist (measure/drawings), line overlays exist
(backplot).

## Numbers become derived, and stay visible

Stepdown = 0.5 × tool diameter · stepover 0.45 · ramp 3° · tabs on any profile
that cuts through. Shown as one sentence — *"6 mm tool, 3 mm deep in 6 passes,
climb"* — with an **Advanced** disclosure holding today's drag fields. Not
deleted; not the first thing you meet.

The one number that is genuinely the user's: **feeds and speeds**. The app
does not know the machine's rigidity, and a wrong guess breaks tools
([plan.md](plan.md)). The tool table keeps asking, and says why.

## The verdict, in words

After Generate, replace "1725 mm of cutting" with what the verifier already
knows:

> ✓ **Machines the part.** No gouges, nothing left over. ~4 min at these feeds.
> ⚠ **Rapid through material at line 42** — raise Safe Z, or start higher.

`cam_verify` produces exactly these numbers; the panel simply does not call it
yet. Verification stops being a thing you remember to run.

## Phases

| | scope | size | |
|---|---|---|---|
| **G1** | `Pick top face` → Z zero / stock top / work origin, and the tool drawn to scale at the pick | ~1 session — *the two mistakes that break tools* | **done** `6ca9351` `6bf67a4` |
| **G2** | The verbs (`Clear a pocket`, `Cut it out`, `Face the top`) on the Tool-window pattern, with derived depths | ~1 session | **done** `8d71991` `43ef325` |
| **G3** | Removed-volume ghost, hover preview, depth label, stock box | ~1 session | **done** `43ef325` |
| **G4** | Plain-language verdict + leftover painted on the part | ~half a session | **done** `43ef325` |
| later | A second setup for flip jobs · toolpath playback · arcs (G2/G3) instead of polygonised curves | on demand | flip **done** (S4, docs/cam/setup-frame.md) |

**`Over openings` passes over HOLES, never over material (2026-08-30).**
Asked as "that sounds like it could be dangerous — what happens if the holes
are smaller than the cutter?", and the sub-cutter hole turned out to be the
safe half: the tool cannot enter a 3 mm circle whatever it does, so crossing
it is harmless and going around it is pure waste. The danger is the opposite
category. A BOSS standing up from the floor has the same kind of inner wire
as a hole and the opposite meaning, and driving over one is a full-width cut
at full depth into material nobody asked to remove. The first cut of this
feature dropped every inner wire, and its own test asserted the cutter driving
through a boss.

The generator tells them apart by sectioning 0.05 mm above the floor: an
inner wire with something standing in it is KEPT whatever the option says. No
flag may point the cutter at material. This is where FreeCAD's `UseOutline`
differs — `Part.Face(element.OuterWire)` loses the bosses with the holes.

Measured, dropping a true island makes the path SHORTER as well as complete:
weaving around one is extra contour on every depth pass, and the sliver it
leaves is not cut by any of it.

**…and an opening the cutter FITS INTO is still an island (2026-08-31).**
`Over openings` crossed every opening, which is right for one the tool cannot
enter — the detour only leaves a sliver — and wasteful for one it can. On a
speaker baffle the flange recess is Ø187.3 with the Ø146.4 driver cutout
through its middle: clearing across that middle is minutes of cutting over a
hole the job removes anyway. Size decides now. On the real part it took the
front side from 26.0 m of cutting to 10.8 m, with the same verdict.

**A hole is not an island — `Over openings` (2026-08-30).** A pocket takes its
boundary from the picked face's wires: the outer one is where to stop, every
inner one an island to go around. Right for material standing in the pocket,
wrong for a HOLE — the cutter detours around thin air, and where a hole nearly
touches the pocket wall it leaves an uncut sliver too narrow for the tool to
come back to. Drilling the holes first does not help: the cut still routes
around where they are. `Over openings` clears the outline and passes straight
across them. FreeCAD spells the same idea `UseOutline` ("uses the outline of
the base geometry", `Path/Op/PocketShape.py`) and implements it the same way —
`Part.Face(element.OuterWire)`, keep the outer wire and drop the rest.

**`Cut it out` on a level FACE, with `side: inside` (2026-09-09).** Cutting
inside a face means cutting the OPENINGS in it — every one, in one operation.
It is the only thing the words can mean: offsetting the face's own outline
inward carves a trench a whisker inside the part, full depth, all the way
round, which is what a face pick silently did until this landed. A boss is
not an opening (the standing-material test `use_outline` already had decides
it), an opening too narrow for the cutter is named in `skipped` while the
others are cut, and `drilled_bores` leaves the round ones up to that
diameter to the drill — which cannot be a width, since a milled slot is
often narrower than a drilled hole (docs/modeling/fpv-drone.md, finding 7).

**`Drill a hole` (2026-08-30).** `OpType::Drill` has been in the engine since
C1, derived from a picked cylindrical face, and reachable only by script. It
is a verb now — and since a verb stays armed, a bolt circle is one click per
hole. Its refusal for a hole that does not face the spindle now says "lay a
face up", which has been the answer since v67.

**The tool glyph is matte (2026-08-30).** It was metallic 0.9 / roughness 0.25
— a near-mirror, so most of what showed on the holder was the environment map
reflected in it, and a reflection slides across a curved surface at its own
rate as the camera orbits. Reported as "the upper part almost follows the
camera but not completely", which is exactly what that looks like; the flute
is thin and mostly buried in material, so it did not show the effect and
"behaved correctly". The glyph is an instrument for reading where the cutter
is, and a mirror finish costs that reading to gain nothing.

**Save G-code wrote into the project (FIXED 2026-08-30).** The comment said
"beside the project"; the code said `projectPath + "/" + name + ".nc"`, and
`projectPath` IS the `.dsgn` bundle — so the G-code landed inside it.
`StampProject` walks every regular file in the project, so the job's own
output changed the project's disk stamp and the next save refused with "the
project changed on disk since you opened it": the app fighting itself over a
file it had just written. It goes in the bundle's PARENT now, named for the
project and the setup (`smart_speaker - Setup 1.nc`) so several parts' jobs
can share a directory.

**`Leave` on an operation (2026-08-30).** `stockToLeave` has been in the
engine since C1 and reachable only by script. It is the roughing/finishing
split: rough at 0.2-0.4 mm, then a second operation at 0 cuts the wall once,
at full depth, in one pass — the difference between a flange a driver seats
flat on and a stack of stepdown terraces.

**Picking, after the fixture frame (2026-08-30).** Three changes, all from
one dogfooding round on a baffle.

*"Pick top face" clamps.* Calling a face the top IS the statement that the
part is held with it up, so the pick lays it up and then takes Z zero from it.
For a face already level `SolveFaceUp` returns the frame it was given, so
nothing moves — the two were only ever separate steps for the faces that used
to be **refused**, and making the user translate a refusal into two more
clicks is not a design. "Lay a face up" stays for the other case: re-clamping
*without* re-answering Z zero, which is what a flip job needs.

*Re-clamping re-derives depths.* `topZ`/`bottomZ` are heights in setup space,
so turning the part left every existing operation measuring from a box that
had moved — a cut that went through the panel became a cut through air. They
follow the frame now, and the count is stated (`state.camNote`) rather than
left to be noticed in the backplot. Same channel says when a Z zero was lost
because the face it came from is no longer level, which used to happen in
complete silence and read as "laying a face flat is broken".

*A verb stays armed.* One verb is almost never one face — a part has six holes
to clear — and disarming on success cost a verb press per hole. The slot
survives the pick; Escape or pressing the verb again puts it away. Pick
buttons are no longer greyed out while another is armed, because a sticky mode
that greys its neighbours means Escape between every change of verb. The
floating window also gained a `Tool` row: every number a verb derives is a
fraction of the cutter, so choosing it afterwards means re-deriving.

**Cutter shape reaches the toolpath (2026-08-31).** It used to stop at the
simulation: `TipHeight` decides what a tool removes, so the verdict and the
picture both knew a ball from a flat mill, and GENERATION did not. Measured on
one pocket, a ball nose and a flat mill produced byte-identical paths — same
5036.75 mm — and the ball left 160 mm² of scallop that only the verifier ever
mentioned. Two things follow from the tip now:

- **Stepover.** A flat mill's floor is flat whatever the step, so 45% of the
  cutter stands. A ball's is not: it leaves a cusp, and the cusp decides the
  step — `s = 2·sqrt(2·r·cusp − cusp²)`, with the cusp target 2% of the tool's
  RADIUS so it stays right for a 1 mm ball and a 12 mm one. On a 6 mm ball
  that is 1.7 mm against a flat mill's 2.7.
- **A cone is refused a floor.** A V-bit or a drill point run along a floor
  ploughs a groove as deep as the pass. There is no stepover that fixes that,
  so pocket, face and adaptive refuse them by name and say what to use instead.

**Concentric rings link at depth (2026-08-31).** Every ring of a clearing
pass used to get a full entry — lift to safe Z, fly back along the circle,
descend, ramp in, cut a lap — so a flange recess did that a dozen times over.
A ring's neighbour starts one step-over away, through the band it is about to
cut anyway, so there is nothing to lift over. `EmitRing` keeps the tool down
when the next ring starts within one and a half step-overs, rotating that ring
to begin where the tool already is; further than that (two disjoint regions at
the same inset) it still lifts, because linking those at depth would drag the
cutter across whatever lies between. On the real job: **121 retracts to 21**,
same verdict.

Two things the change needed that are worth knowing. A lap must be able to end
WITHOUT retracting, or the next ring never finds the tool at depth — and once
laps end down, the next entry's lateral rapid starts at the bottom of a cut,
so it has to climb straight up before it travels. Both were caught by the
existing gates rather than by looking (`cam: a pass never rapids to a depth
nothing cleared` went to 14 the moment the retract came out).

**A profile follows the part's projection, not its section (2026-08-31).**
A cutter at depth z removes the whole column above it, so it may only travel
where every height between the operation's top and z is waste. The contour is
therefore the part's projection over [z, topZ] — the union of its sections —
and not the section at z, which is a different shape wherever a feature opens
part-way down. A cross-dowel bore opening onto a panel's side face notches the
section at its own height and nowhere else; following that notch cut the
bore's 8 mm plus the solid above and below it, an 8.00 mm gouge on the real
part. `ProjectedOutlines` accumulates it down the passes; a picked wall is
re-resolved against it per pass rather than named once at the wall's own
mid-height. One rule serves both sides — the sweepable region is the
complement of the projection — and it accumulates from the OPERATION's top,
not the stock's, since that is the declaration that what is above is already
clear. Gated in both directions: a notch under solid material is never
entered, and a notch open from the top still is.

**Backplot contrast (2026-08-31).** Pale blue on a light grey workpiece is a
hair off the surface it is drawn on. Cut moves are amber now, plunges red,
rapids stay muted grey — amber reads against both the part and the dark
ground, and is clear of the blues selection and sketches use.

**Stock and Zero (2026-08-30, v68).** "Stock and safety" gained the two
things a Fusion setup has and this one did not. *What the workpiece is*:
"Around the part" (the model box plus margins — what every job had, still the
default) or "A fixed sheet" with its real size and where the part sits in it.
A sheet smaller than the part is refused, naming the axis and the shortfall,
because "get a bigger board" and "draw something" used to arrive as the same
sentence. *Where zero is*: the corner the operator touches off on — stock
corner, stock centre or part corner — with the derived triple printed beside
it. The post SUBTRACTS it, so the file's zero is that corner at the height of
the picked top face. Both are scripted by `set_cam_setup {origin_at, stock}`.

**The operation row (2026-08-30).** Two things it could not say. *Which
cutter*: `op.toolNumber` was read to print a diameter and never written, so
"this opening is 5.46 mm across and tool 1 is 6 mm — use a cutter of 5.46 mm
or less" was a refusal naming a remedy the panel could not perform, with the
smaller tool already defined in the setup. It is a row of `T1 T2 …` buttons
under Advanced — buttons rather than a combo because a setup has a handful of
tools, so one click beats two, and because a combo gathers with an empty
label and could not be gated. Retooling carries the stepdown: half the new
cutter, unless you typed your own. *Which order*: the operations list had no
reordering at all, though the order IS the cutting sequence — the generator
and the post both walk the vector, so it decides what is roughed before what
is finished and where the tool changes and their `M0` pauses fall. Drag the
grip handle. Scripted twin: `move_cam_op {id, to}`.

G1 first: picking the top face is the highest-value single click in CAM, and
the tool-to-scale drawing prevents the most common beginner surprise.

## What this subsumes

Two papercuts already owed from using the panel, both of which evaporate:

- the pocket refusal that says *"check its depths against the model"* instead
  of naming the model's Z span — with a picked floor there is nothing to
  refuse;
- pocket defaults derived from the stock box (what produced `2 → -0`) — the
  picked face supplies them.

## Verification

- Headless: picking a face yields an operation whose depths match that face
  and the stock — no field is read from the panel.
- GUI: two clicks (verb, face) produce a pocket op with a non-empty toolpath;
  a pick on a curved face refuses visibly; closing the session drops the
  ghosts but keeps the backplot; **and the id-conflict guard stays** (two
  tools + two operations, after `3c873a7`). R15 adds the same pair for the
  edge break (`cam/verbs_two_clicks_to_an_edge_break`): with the setup's
  default FLAT tool the pick refuses and makes no operation; with a ball nose
  it derives the round from the wall and the tool.
- The verdict text is asserted against a job with a known gouge, so the words
  cannot drift from what the verifier reports.

*As built:* `tests/test_cam.cpp` holds the derivation tests (a clicked floor
gives every depth; an island in that floor is machined around; each verb that
cannot apply says why; leftover means unreachable, not unprogrammed) and
`GuiTests.cpp` `cam/verbs_two_clicks_to_a_pocket` walks the whole path
including the ghost's Z span and the session close.

## As-built

Everything above shipped. What the design did not anticipate:

**Three defects the first screenshot found**, none of which a test was
looking for:

1. **Safe Z was a bare 5 mm** — an absolute Z, so it sat *inside* any stock
   taller than 5 mm, and the first job made by clicking rapided straight
   through the block. The verifier reported it ("rapid through material at
   line 11") the moment G4 started running automatically, which is the whole
   argument for G4 in one line. Now `cam::DeriveSafeZ` puts it above the
   stock top, and picking the top face re-derives it because the stock just
   changed shape.
2. **Leftover was measured over the whole stock.** A job that clears one
   pocket in a block leaves the rest of the block untouched *correctly*, and
   the verdict called all of it "22 mm of material left" and painted the part
   solid red. Leftover is now measured only where the tool actually went
   (`Inspection::touched`); the separate signal — a program that machines
   nothing at all — moved to `Verdict::untouchedFraction`.
3. **The UI font stopped at Latin-1.** Every em-dash and ellipsis in the
   app's own prose rendered as `?`, in CAM's refusals and equally in the
   section tool's "source missing — this cut is off". Fixed in `Theme.cpp`
   for the whole app rather than by policing strings; verdict ticks use the
   already-merged icon font, which the text font still has no glyph for.

**One the GUI suite found:** the CAM panel was the only panel tabbed into the
left dock node without `NoFocusOnAppearing`, so on first launch it took the
tab from the model tree. Three unrelated tests had been failing on it
("unable to hover Model"); a user would have said their tree was gone.

**A convention that used to be worth knowing, resolved:** CAM's Z is the
spindle axis, and the app's world is Z-up since docs/up-axis.md (2026-08-28)
— the plan view for a 3-axis job simply IS the Top view, and picking "the
top face" from the Top view derives an operation instead of being refused as
a wall. "Look down the spindle" remains for any setup whose spindle is not
the world's up — an indexed one, and since v67 a part clamped flat that is
not modelled that way (docs/cam/setup-frame.md).

Deliberately still open at v1, and **two of the three have since shipped**
(CAM phase 2, 2026-08-21): helix entry for pockets too small to ramp into is
**C6** (`b00cbf9`), and arcs in the posted file instead of polygonised curves
are **C7** (`6c73990`, flat arcs; helical deferred). Still open: the ~0.4 %
cut-length overcount at the ramp seam.

### The verbs became the toolbar (2026-09-02)

The design above put the verbs in **one floating window** opened by a
toolbar button, because CAM lived on the part and a part's toolbar was full.
Since the job became its own document kind ([job-document.md](job-document.md))
that reads wrong on both sides: a job tab was drawing the *part* bar — thirty
modelling tools, every one disabled — and the way into CAM was a button that
opened a list of buttons.

Now a job tab has **its own toolbar**, and the verbs are its tools, one
button each, the way the part bar has one per feature:

```
[Z zero] [Lay face up] [Turn face up*] [Look down spindle] |
[Clear a pocket] [Cut it out] [Drill a hole] [Break an edge] [Whole part] [Face top] |
[Generate] [Simulate] | [Fit] [Measure]        * only on a setup with a rotary axis
```

Every button is a command (`job.*`, keymap context **`job`**), so the keys,
the palette and the tooltips agree with the bar. Pressing a verb opens **that
verb's window** — its sentence, its prompt, and **the tool it derives for**,
with the operation it made under it — and lights the button while the pick
is armed; pressing it again puts it away. The two no-pick verbs make the cut
at once and open their window so the tool can still be changed: choosing
another tool **re-derives the cut just made** rather than asking for a
remove-and-redo.

**The tool defaults to the last one used** (`ui::VerbTool`): the one chosen
this session, else the setup's newest operation's, else the first. It is
read from the job's operations, so it holds across sessions.

The CAM panel keeps the **list** — setups, parts, stock, tools, operations,
the transport — and now exists **only on a job tab**; the "Pick top face"
row and the "Verbs" button left it for the bar. On a part the way in is
still the part bar's own CAM button ("machine this part"), which opens the
job tab.

**One the change found:** the cutting simulation was drawn in *model* space
— the stock and the cutter multiplied by setup-to-model — in a viewport that
draws *setup* space. Invisible while a setup had one frame, permanent once
fixturing became per part (v76): the block stood upright through a part
lying flat. Same bug the backplot had the same week; the transform is
deleted rather than corrected, and `backplot_lands_on_a_clamped_part` now
asserts the cutter is drawn exactly at the timeline's cursor.

**2026-09-02, later:** the part bar's CAM button is gone. A job is made from
the tab bar's `+` and set up in the panel — machine, stock, parts — the way
an assembly is made and then filled ([job-document.md](job-document.md),
round two, M3).

