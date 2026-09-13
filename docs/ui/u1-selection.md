# U1 — Unified selection model (design)

> Status: **SHIPPED 2026-07-18** — all four phases (U1a `eaa4fc1`, U1b
> `754d4d0`, U1c `5e3ed9a`, U1d). SW/Onshape conventions as signed off.
> Deliberate carve-out: mate-connector picking keeps its own picker (it
> resolves (occurrence, face-frame) pairs the generic candidates don't
> carry). The single face-highlight slot and all-depth candidate enumeration
> remain explicit U1e follow-up work below; occurrence hover still uses the hit
> face's range.
> Parent: [plan.md](plan.md) U1.
> Scope: model-space selection. The 2D sketcher's entity selection is mature
> and stays as-is; it integrates only at the prompt/filter level.

## Follow-up: candidate completeness and verification

The U1a–d migration is shipped, but its U1b disambiguation promise is only
partially met. `SelectionPipeline::Candidates` currently receives only the
nearest face/edge result from `FaceSelector`; repeat-click cycling can switch
between that edge, face, and body, but cannot reach a face behind the first
one. Treat this as a focused U1e follow-up, not a second selection model.

1. **All-hit candidate API.** Extend the picking engine to return every eligible
   ray hit with depth; preserve the priority rule Edge > Face at equal depth,
   then sort by depth. Feed that list into the existing stable-item pipeline
   and repeat-click cycle.
2. **Projection completeness.** ~~Make multiple selected faces on the same
   renderer instance visible simultaneously rather than retaining only the
   most recently synchronized face range.~~ **DONE 2026-07-27** (`3c22fcc`,
   with U7): per-instance range list + discard-outside sub-draws.
3. **Acceptance coverage.** Add GUI tests for two stacked faces cycling to the
   hidden face, an armed blend/slot prompt and filter flow, and an
   assembly-occurrence click/gizmo flow. Keep the existing stable-ID and
   default part-picking tests as regression coverage.

## What exists today (audit)

Selection state is scattered across seven mechanisms:

- **Feature selection:** `AppState::selectedFeature` (an **int index**) could
  dangle across delete/undo; `DrawUI` clamped it every frame, while RPC, tree,
  and properties all read the raw index.
- **Face selection:** `FaceSelector::m_selected*` plus duplicated
  `MeshRenderer` highlight state allowed one face only; the pick engine owned
  selection and three call sites synchronized renderer highlights by hand.
- **Edge picking:** `EdgePickState` existed only for fillet/chamfer collection;
  edges were otherwise unselectable.
- **Measure:** `MeasureState` owned its element pair and pick handler.
- **Occurrence / mate:** `selectedOccurrence` and `selectedMate` were separate
  assembly-tab flows.
- **Simulation BC / keep faces:** `SimulationState::tool` had its own armed
  pick path.
- **Mate connector:** `MateToolState` had a separate ray and resolver.

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

### Lists — one gesture, one Delete (2026-09-02)

Five lists now: features in a part, occurrences, mates and relations in an
assembly, operations in a job. The three the section was written for had
drifted: features selected one
at a time, occurrences Ctrl-clicked, operations had no selection at all and
an `x` per row; two had a right-click Delete, one had the Delete key. What
is shared now is the GESTURE and the VERB (`src/ui/ListSelection.h`), not
the row: a feature row carries a rollback bar and Suppress, an occurrence
row a visibility box, an operation row an enabled box and a ghost button,
so a shared row widget would be a configuration blob that also renames the
item ids the GUI tests address.

- `ListRowClicked(kind, id, order)`: plain click selects the row alone and
  becomes the range anchor (`Selection::rangeAnchor`, per kind); Ctrl
  toggles it (and moves the anchor, as a file manager does); Shift selects
  from the anchor to the row over `order` — the rows as the panel lists
  them — replacing that kind; Ctrl+Shift adds the range.
- `clickClears` (opt-in, the MATE list takes it): a plain click on the row
  that is the whole selection of its kind clears it. For a list whose
  selected row opens an editor INSIDE the panel — the selected mate's name,
  flip, DOF fields and connectors — because the row you just clicked is the
  only affordance there and it used to do nothing, so the editor could be
  changed but never put away. Off everywhere else on purpose: most lists
  auto-select what they just created (`ToolbarCreateAccept` leaves the new
  feature selected), and clicking that row to confirm what you are looking
  at must not make it vanish. `selection/survives_sibling_delete` is the
  test that says so.
- `ListRowContext`: right-click inside the selection keeps it; outside, the
  row becomes the selection. The menu's Delete says how many
  (`DeleteLabel`: "Delete 3 features").
- `DeleteSelection`: the one Delete — features through
  `RemoveFeaturesCommand` (one undo step, one regenerate), occurrences
  through `DeleteSelectedOccurrences` (one snapshot, one solve), operations
  through `RemoveCamOperations` (both CAM windows had their own remove with
  different side effects; now one). On an assembly tab it takes MATES and
  occurrences together in ONE snapshot (`DeleteSelectedOccurrences` grew a
  `takeSnapshot` flag so the caller can own it): an occurrence click
  deliberately keeps the selected mate, so the two selections coexist
  routinely, and honouring one of them would mean Delete silently ignored
  something the panel was showing as picked. The Delete key calls it from the
  modal-key chain in ViewportInput, yielding to armed slots, a mate or
  fastener session, a text field, a sketch being edited and an open editor.
- Selection got an `Operation` kind for the job's operation rows (id = the
  operation's id), pruned like the others.

A range delete of features can leave a dependent broken (an extrude whose
sketch went); it does, and the row turns red with the reason — exactly as a
single delete always has, so nothing is refused that used to be allowed.

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
- **U1c — armed tools become filters.** Measure, blend edge collection and
  sim BC/keep rewired as filter + consumer; their private ray/resolve code
  deletes; the model-space prompt bar shows each one's prompt. The part-tab
  `ViewportInput` chain collapses to: sketch → tool lifecycle (Escape/
  disarm) → the one pipeline. *(The mate connector moved to U1d with the
  assembly fold-in — the pipeline doesn't run on assembly tabs until then.)*
  *Tests: existing measure/fillet/BC gui cases stay green (the real
  regression gate); prompt pill shows per armed tool.*
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
