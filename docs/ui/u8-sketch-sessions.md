# U8 — Sketch sessions: the sketcher joins the uniform interface (design)

> Status: **DESIGN — signed off 2026-07-27.** Decisions: (a) the Sketch
> options panel is DELETED (tool sessions move to the Tool window, other
> tenants disperse); (b) the sketch entity-selection store stays internal —
> unification happens at the session/prompt/seeding level, not by merging
> stores; (c) one-time layout reset (dockspace id bump) accepted to kill
> stale-ini zombie dock nodes.
> Parent: [plan.md](plan.md) U8 · builds on [u7-tool-sessions.md](u7-tool-sessions.md).

## Problem (audit 2026-07-27)

Sketch mode's *conventions* already match U1 (click/Ctrl/cycle/box select,
staged Escape ladder) but the machinery is a parallel universe:

- Private selection store: `SketchEditState::selected`
  (`vector<sketch::PointRef>`) + ~8 satellite fields; the vector is
  overloaded as the Dimension target list and Mirror/Pattern/Offset source.
- Hand-rolled sessions: Mirror is a two-slot session driven by a bespoke
  `mirrorAxisPhase` bool; Pattern/Offset are selection+params+Apply
  sessions with Apply buttons in the docked panel; Calibrate has its own
  `calibStage`.
- Duplicate prompt pill: `SketchToolPrompt`/`DrawToolPrompt` parallel to
  `DrawModelToolPrompt`; they mutually exclude in UI.cpp.
- No seeding: sketch Extrude/Revolve ignore the selection and always take
  the whole sketch.
- The "Sketch" panel is thin (auto-constrain, images, a Status section that
  duplicates the DOF pill, profile counts, Extrude/Revolve/Exit) and — via
  stale imgui.ini dock ids — resurrects the right dock node U7e deleted
  (dock-home enforcement only rescues FLOATING windows).

Blast radius: zero RPC/MCP coupling; ~20 gui-test bodies poke
`edit.selected` / `ApplySketchConstraint` directly (they survive since the
store stays); 2 tests SetRef("//Sketch") and re-target moved buttons.

## Design

### U8a — one surface: sketch tool sessions, panel deleted

The active sketch tool's UI moves into the floating `###ToolWindow`, same
look as model sessions (slot-style rows, params, Apply/Cancel):

| Tool | Session window content |
|---|---|
| Mirror | Geometry row (N selected; armed in phase 1) · Axis row (armed in `mirrorAxisPhase`) · Linked checkbox |
| Fillet | Radius |
| Pattern | Geometry row · Linear/Circular · count · step/centre/angle · Linked · Apply |
| Offset | Loop row (selection) · Distance · Round corners · Apply |
| Calibrate | picked span · distance · align · Apply / Re-pick |
| Dimension | accumulated target rows (per-item x) |
| Select / drawing tools | no window — the prompt pill is enough |

Panel tenants disperse:
- **Status/DOF/conflicts** → deleted (the pill already shows it).
- **Auto-constrain, Show constraints** → toolbar toggles in the sketch row.
- **Reference images, Constraints list, Patterns (linked ops), Linked
  copies** → sections at the bottom of the Model panel while editing
  (same pattern as mass properties).
- **Extrude… / Revolve… / Exit** → the top sketch toolbar row.
- `Begin("Sketch")` deletes; DockLayout loses its Sketch references.

**Zombie-node fix:** the dockspace id gains a version suffix
("MainDockSpaceV2"): an ini from the old layout has no entry for the new
id, so the default layout rebuilds once and orphaned nodes die.

### U8b — one prompt implementation + selection→Profiles seeding

- One shared pill renderer serves both `DrawToolPrompt` (sketch: DOF pill,
  staged step prompt, actionMsg) and `DrawModelToolPrompt` (armed prompt,
  actionMsg) — the stagers stay per-mode, the drawing dedupes.
- `BeginSketchExtrude`/`BeginSketchRevolve` seed the Profiles slot: loops
  containing any selected entity become the feature's `regions` (none
  selected = whole sketch, as today). This closes the select-profile-first
  workflow gap.

### Out of scope (deliberate)

Merging `edit.selected` into `AppState::selection` (U8c if ever): the store
already behaves per U1 conventions, is load-bearing for drag sessions and
constraint argument order, and the merge is the widest-risk change in the
app for a purely internal win.

## Phases

- **U8-0** dockspace version bump (ships alone; one-time layout reset).
- **U8a** sessions + panel dissolution. *Tests: existing sketch suites
  green with re-targeted paths; new case: pattern Apply via the Tool
  window.*
- **U8b** shared pills + seeding. *Tests: two-region sketch, select one
  loop's entity, Extrude → regions == that loop.*

## Risks

- Tests clicking panel buttons re-target (2 SetRef("//Sketch") sites +
  any moved Apply buttons).
- The Tool window shows/hides with tool changes mid-sketch — its
  ImGuiCond_Appearing anchor re-fires per session, which is the intended
  behavior (same as model tools).
- Toolbar row width grows (Extrude/Revolve/toggles); the row already
  scrolls horizontally on overflow (U4).
