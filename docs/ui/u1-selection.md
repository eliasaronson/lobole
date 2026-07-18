# U1 — Unified selection model (design)

> Status: **signed off 2026-07-18** (phases U1a–U1d; SW/Onshape conventions:
> click replaces, Ctrl+click toggles, repeated-click cycles — no popup in
> v1). Parent: [plan.md](plan.md) U1.
> Scope: model-space selection. The 2D sketcher's entity selection is mature
> and stays as-is; it integrates only at the prompt/filter level.

## What exists today (audit)

Selection state is scattered across seven mechanisms:

| Mechanism | State | Problems |
|---|---|---|
| Feature selection | `AppState::selectedFeature` (**int index**) | Dangles across delete/undo — `DrawUI` clamps it every frame; RPC + tree + properties all read the raw index |
| Face selection | `FaceSelector::m_selected*` + duplicated highlight state in `MeshRenderer` | Single face only; selection lives in the *picking engine*; three call sites keep renderer highlights in sync by hand |
| Edge picking | `EdgePickState` (fillet/chamfer collection) | Hover + click exist **only** while a blend is collecting; edges are otherwise unselectable |
| Measure | `MeasureState` (own element pair) | Own pick path, own click handler |
| Occurrence / mate | `selectedOccurrence`, `selectedMate` (ids) | Assembly-tab-only, separate flows |
| Sim BC / keep faces | `SimulationState::tool` armed-pick | Own ray + resolve + one-shot disarm |
| Mate connector | `MateToolState` | Own ray + resolve |

`ViewportInput.cpp` routes clicks through a **priority chain** of these
handlers; each computes its own ray, resolves `documentMeshes → Body →
ElementRef` by hand, and manages its own hover. Every new tool has added one
more branch (this is how the chain got seven deep).

## Design

### The selection set

```cpp
// One selected thing, stable across regeneration: geometry is referenced by
// stable element name (toponaming), never by mesh/face index — mesh ids
// change on every regen.
struct SelectionItem {
  enum class Kind : uint8_t { Face, Edge, Body, Feature, Occurrence, Mate,
                              SketchRegion /* U2 */ };
  Kind kind;
  uint64_t id = 0;    // Feature/Occurrence/Mate id; Body: createdBy
  ElementRef element; // Face/Edge/SketchRegion: {featureId, stable name}
};

struct Selection {          // AppState::selection — THE source of truth
  std::vector<SelectionItem> items; // ordered: pick order matters to tools
  // Toggle/Replace/Clear/Has(kind)/FirstOfKind helpers
};
```

Renderer highlights become a **projection of this set**, applied in one sync
point per frame (like the sketch overlay rebuild) — no more paired
`SetSelectedFace`/`SetFaceHighlight` calls at every site.

### Filters — what a context accepts

```cpp
struct SelectionFilter {
  uint32_t kinds = 0;     // Kind bitmask the active context accepts
  int maxItems = -1;      // e.g. measure = 2, mate connector = 2
  std::string prompt;     // status pill: "Pick a face or edge (Esc exits)"
};
```

Whoever arms a mode installs the filter (measure, blend edge collection, sim
BC/keep, mate tool; U3's dialog pick slots install these per slot). No armed
filter = the tab's default (part: Face|Edge|Body|Feature; assembly:
Occurrence|Mate). Picking auto-filters candidates; the **prompt bar** — the
sketcher's pill renderer generalized to model space — says what's expected
and shows `actionMsg` feedback.

### One pick pipeline

A single function per frame builds the ray once and queries **all**
candidates the filter allows — `PickFace`, `PickEdge`, occurrence resolution
— ranked by (kind priority at equal depth: Edge > Face; then depth). It
produces:

- **Hover**: what a click would take, highlighted every frame (faces have
  this today; edges only during blends; occurrences never).
- **Click**: plain click replaces the set; **Ctrl+click toggles**
  (multi-select). Armed filters route the resolved item to the armed
  consumer instead (one `pickConsumer` callback in AppState), which is how
  measure/blend/BC/mate shed their private ray code.
- **Disambiguation**: clicking again within a few px **cycles** to the next
  candidate under the cursor (face behind face, edge vs coplanar face) —
  rank order, wrap-around. (A "select other…" popup can come later if
  cycling proves insufficient; not in v1.)

`FaceSelector` keeps its role as the *pick engine* (ray queries, face data)
but stops owning selection state.

### Consumers migrate, not duplicate

- `selectedFeature` int → `SelectionItem{Feature, id}`. Kills the
  dangling-index clamp; tree/properties/RPC read through one helper.
- `get_state` reports the typed list under `selection`
  (`[{kind, id, element}...]`); the legacy `selected_feature` int and the
  face block stay during migration. `select_face`/`select_feature` drive the
  new set (names unchanged).
- Escape clears armed filter first, then selection (matches today's
  tool-Escape cascade).

## Out of scope

Sketch-entity selection (mature); region picking (U2 adds
`Kind::SketchRegion` + the shaded-region hit test); dialog pick slots (U3 —
they *install* filters, which is why U1 precedes it); box/lasso select in 3D
(revisit at U5); drawing-sheet selection (own model, D-series).

## Phases

- **U1a — the type + feature selection.** `Selection` lands in AppState;
  feature selection migrates to stable id (tree, properties, RPCs,
  delete/undo paths). Mechanical, small, immediately kills the index-dangle
  class. *Test: select → rename → undo → selection still on the same
  feature; delete clears.*
- **U1b — faces/edges/bodies through the one pipeline.** Ray built once;
  hover preview for edges everywhere; Ctrl+click multi-select;
  disambiguation cycling; renderer highlights become a Selection projection;
  `get_state` grows the typed list. *Tests: edge hover outside blend mode;
  two stacked faces → second click cycles to the hidden one; Ctrl+click
  builds a 2-item set.*
- **U1c — armed tools become filters.** Measure, blend edge collection, sim
  BC/keep, mate connector rewired as filter + consumer; their private
  ray/resolve code deletes; the model-space prompt bar shows each one's
  prompt. The `ViewportInput` chain collapses to: sketch → gizmo → the one
  pipeline. *Tests: existing measure/fillet/BC/mate gui cases stay green
  (the real regression gate); prompt pill shows per armed tool.*
- **U1d — assembly kinds.** Occurrence/mate selection folds into the set
  (click occurrence = `{Occurrence, id}`; mate list selection ditto);
  `selectedOccurrence`/`selectedMate` delete. *Test: assembly click/gizmo
  flows stay green.*

Each phase ships alone and keeps `gui_smoke` green (43 cases — run via
`tools/gui-tests-xvfb.sh`, immune to screen lock).

## Risks

The regression surface is every picking flow in the app; that is also the
payoff. Mitigations: phase order defers the riskiest rewiring (U1c) until
the pipeline is proven on plain selection (U1b); FaceSelector's engine API
stays stable; the gui suite already covers face pick, edge pick, measure,
BC and assembly flows.

## Sign-off questions

1. Phasing U1a→U1d as above?
2. Multi-select = plain click replaces, Ctrl+click toggles (SW/Onshape
   convention)?
3. Disambiguation = repeated-click cycling (no popup in v1)?
4. Fold occurrence/mate into the same set (U1d) rather than leaving the
   assembly tab's selection separate?
