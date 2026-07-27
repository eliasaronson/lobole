# U7 — Tool sessions: one pattern for every pick-collecting tool (design)

> Status: **DESIGN — direction signed off 2026-07-27.** Decisions recorded:
> (a) the floating session window is THE tool home and the right Properties
> panel is **removed entirely**; (b) pre-selection seeds slots greedily in
> slot order, no disambiguation prompt in v1; (c) mate connectors fold into
> `SelectionItem` as `Kind::Connector`.
> Parent: [plan.md](plan.md) U7.
> Scope: model/assembly/drawing tool UIs. The 2D sketcher's internals stay
> as-is (it is already select-first); only its options panel re-homes.

## Problem (audit 2026-07-27)

U1 gave one typed selection set + one ranked pick pipeline; U3 gave slot
rows. But only four tool families use them (`ActiveArmedContext()` in
`SelectionPipeline.cpp`: edgePick, slotPick, measure, simulation), and the
rest are parallel universes:

- **Mate tool**: own state, own ray (`MateToolPick`), invisible two-click
  sequence dispatched inline in `ViewportInput.cpp` — bypasses `Selection`,
  `Candidates()`, and the armed-context system entirely.
- **Drawing dim tool**: own 2D pick loop + ref store; no per-item removal;
  Esc discards the whole in-progress dim.
- **Two surfaces render the same widgets**: the floating bottom-center
  preview dialog (`DrawPreviewDialog`) and the docked Properties panel both
  call `DrawFeatureParamWidgets`. The floating one has twice broken GUI
  tests by covering viewport clicks.
- **No select-then-tool in model space**: `BeginPreviewFeature` *clears* the
  selection (`PreviewFeature.cpp`), so every tool starts empty even when the
  user pre-selected exactly the right things.
- **Editing is inconsistent**: fillet/shell/draft/measure have per-row "x"
  lists (copy-pasted four times); single-value slots silently overwrite;
  mate and dims can only cancel-and-restart.
- **Arming UI is boilerplate**: "button toggles a bool + pushes active
  style" exists independently in Toolbar, StudyPanel, DrawingPanel, and
  FeatureEditor.

## Design

### Declarative slots — a tool is a list of slot specs

```cpp
struct SlotSpec {
  const char* name;      // "Profiles", "Mirror plane", "Connector A"
  uint32_t kinds;        // SelectionItem::Kind bitmask (existing KindBit)
  int arity;             // 1, or -1 = unbounded multi; N = capped multi
  bool required = true;
  const char* prompt;    // status pill text while this slot is armed
};

struct ToolSession {                       // AppState::toolSession (optional)
  // Which tool: feature preview (pendingFeatureId), committed-feature edit,
  // measure, sim load, mate, drawing dim — one enum + payload id.
  std::vector<SlotSpec> slots;
  std::vector<std::vector<SelectionItem>> filled; // parallel to slots
  int activeSlot = 0;
};
```

`ActiveArmedContext()` stops being a hand-maintained priority chain over
four structs: it derives from *the active slot of the active session* —
`{kinds, prompt, consume=route-into-filled[activeSlot]}`. `SlotPickState`,
`EdgePickState`, the measure/sim arming flags, and `DimTool`'s ref
accumulation all collapse into this. (Their domain state — measure readout,
sim load params — stays; only pick collection unifies.)

### The session window — one home for every tool

One floating ImGui window (`"Tool"`), shown iff a session is active:

- **Anchor top-right by default**, position remembered per run, draggable.
  **Never bottom-center** — that placement is the proven click-occluder.
- Header: tool name + ✓/✗ (Enter/Esc equivalents, same cascade as today).
- **Slot rows** via one shared widget (replaces the four copy-pasted list
  UIs): each filled item is a chip with an "x"; the row shows its filter
  ("2 edges"), clicking a row arms it, the armed row is highlighted, and
  the prompt bar mirrors its prompt. Single slots get the same treatment —
  value shown, click to re-pick, x to clear. Nothing is overwrite-only.
  Slot ids stay `"###slot<N>"`-stable for tests.
- Params below the slots: the existing `DrawFeatureParamWidgets` content
  (numeric expressions, mode toggles, combos as the precision path).

**Committed features open the same window**: double-click in the tree (or
context-menu Edit) starts a session seeded from the feature's stored refs.
The always-visible Properties param editing goes away with the panel — edit
becomes an explicit, uniform action (SW/Onshape model).

### Active slot, advancement, switching

Exactly one active slot at a time. Picks route into it. `arity == 1` slots
auto-advance to the next unfilled *required* slot (boolean stays
click-click-done). Multi slots stay armed — toggle on re-click (existing
`ToggleRefInList` semantics) — until the user clicks another row, Enter
(accept), or Esc (disarm slot first, then cancel session; today's cascade).

### Seeding from pre-selection (select first, then tool)

`BeginPreviewFeature` stops clearing the selection. Session start runs a
seed pass: walk slots in declared order; for each, greedily take the
oldest-picked `Selection` items matching its `kinds` mask up to `arity`.
Consumed items leave the global set (they now live in the session); a wrong
guess is a one-click fix because every slot is editable. No disambiguation
prompt in v1. Toolbar/palette actions may also *gate or specialize* on the
current selection the way the sketch toolbar already does (e.g. edges
selected → Fillet opens fully seeded; a construction-vs-normal distinction
rides on the item's kind/flags).

### Connectors become a selection kind

`SelectionItem` gains `Kind::Connector`: `id` = occurrenceId, `element` =
the face ref; the face-frame (origin/axis, hole-center snap) is resolved at
consume/preview time as today. `Candidates()` produces connector candidates
on assembly tabs when a slot asks for them; `MateToolPick`,
`UpdateMateToolHover`, and the inline dispatch in `ViewportInput.cpp`
delete. Mate = two required connector slots + mate-type params; editing a
mate re-opens the session (today: cancel-and-restart only).

### Right panel removal

With sessions floating, the right dock node's tenants disperse:

| Today in Properties | Goes to |
|---|---|
| Selected-feature param editing | the session window (explicit edit) |
| Measure list + readout | the measure session window |
| Legacy "Selection" face/normal readout | **deleted** (pre-U1 FaceSelector mirror) |
| Camera position/distance | **deleted** (debug leftover) |
| Mass properties (+ CoG marker) | collapsing section in the Model panel |
| "Sketch" options panel (tabbed right) | docks **left**, tabbed with Model, while sketch edit is active |

`DockLayout.cpp` drops the right split (viewport gains ~22% width);
dock-home enforcement and Reset Layout update accordingly.

## Migration map

| Tool | Slots (kinds, arity) |
|---|---|
| Extrude / Revolve | Profiles (SketchRegion, multi) · up-to Face (1, optional) |
| Boolean | Target (Body, 1) · Tool (Body, 1) |
| Pattern / Mirror | Source (Body, 1) · axis/plane refs (combo or Face slot) |
| Shell | Body (1) · Open faces (Face, multi) |
| Draft | Faces (Face, multi) |
| Fillet / Chamfer | Edges (Edge, multi) — `EdgePickState` deletes |
| Rib / Thread | Body / Face (1) |
| Mate | Connector A (1) · Connector B (1) |
| Measure | Elements (Face\|Edge, multi; existing toggle+readout logic) |
| Sim loads / keep | Faces (Face, multi) + load params; accept applies |
| Drawing dims | Refs (2D curve refs, arity from dim kind) — gains per-row x |

## Out of scope

Sketch-mode internals (mature, already select-first; its selection may seed
the Profiles slot in a later pass — stores stay separate). U1e depth-cycling
completeness (independent follow-up). Drawing-sheet *selection model* beyond
the dim tool's session. Box/lasso select.

## Phases

- **U7a — the frame.** `SlotSpec`/`ToolSession` + shared slot-row widget;
  existing U3 dialogs re-expressed declaratively; the floating session
  window replaces `DrawPreviewDialog` (top-right anchor). Properties panel
  still exists, untouched. *Tests: all existing dialog-slot gui cases green
  with updated window path; new case: session window never intersects the
  viewport center region.*
- **U7b — seeding.** Kill the clear in `BeginPreviewFeature`; seed pass;
  selection-aware toolbar gating. *Tests: select two bodies → Boolean opens
  with target+tool filled, Enter commits; select edges → Fillet seeded;
  wrong-slot seed is correctable via row click without restart.*
- **U7c — mate.** `Kind::Connector` in `Candidates()`; mate = two-slot
  session; private picker + ViewportInput bypass delete; mate *edit*
  re-opens the session. *Tests: existing mate gui flows green through the
  pipeline; edit-a-mate case.*
- **U7d — stragglers.** Measure, sim loads/keep, drawing dims become
  sessions; their arming boilerplate deletes. *Tests: existing measure/BC/
  dim cases green; dim ref removal case.*
- **U7e — right panel removal.** Relocations per the table; dock layout
  drops the right node; committed-feature edit via double-click/context
  menu everywhere. *Tests: no-overlap layout case updated; tree
  double-click opens session for a committed feature; mass-props section in
  Model panel.*

Each phase ships alone and keeps the gui suite green
(`tools/gui-tests-xvfb.sh`).

## Risks

- **Test-id churn**: every gui case addressing `//Properties/...` or the
  preview dialog re-targets the `Tool` window — mechanical but wide; do it
  per phase, not in one sweep.
- **Behavior change**: committed-feature params are no longer passively
  visible on tree selection; edit is explicit. Accepted as part of (a).
- **Floating occlusion**: mitigated by the anchor rule + the U7a
  no-center-overlap test; tests keep clicking top faces per the U2 gotcha.
- **Mate regression**: U7c rewires the one picker with no pipeline
  coverage today; port its hover/preview behaviors behind the existing mate
  gui cases before deleting the old path.
