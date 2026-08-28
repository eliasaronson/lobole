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
| later | A second setup for flip jobs · toolpath playback · arcs (G2/G3) instead of polygonised curves | on demand | |

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
a wall. "Look down the spindle" remains for indexed setups, whose spindle is
not the world's up.

Deliberately still open at v1, and **two of the three have since shipped**
(CAM phase 2, 2026-08-21): helix entry for pockets too small to ramp into is
**C6** (`b00cbf9`), and arcs in the posted file instead of polygonised curves
are **C7** (`6c73990`, flat arcs; helical deferred). Still open: the ~0.4 %
cut-length overcount at the ramp seam.
