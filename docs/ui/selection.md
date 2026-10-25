# Selection and picking

## One selection set

`AppState::selection` (`struct Selection`, `src/ui/AppState.h`) is the one
typed, ordered set every context reads and writes. Items are stable across
regeneration: features, occurrences, mates, relations and operations by id,
geometry by stable element name (toponaming) — never by mesh or face index,
which change on every regeneration.

```cpp
struct SelectionItem {
  enum class Kind { Face, Edge, Body, Feature, Occurrence, Mate, Relation,
                    SketchRegion, Connector, Operation, SketchCurve,
                    CamSetup, JobPart, Fixture, CamTool };
  Kind kind;
  uint64_t id;         // feature/occurrence/mate/relation/operation/
                       // setup/job part/fixture id; CamTool: the active
                       // setup's tool NUMBER; Body: its createdBy;
                       // Connector: the occurrence
  ElementRef element;  // Face/Edge/SketchRegion/Connector: {featureId, name};
                       // SketchCurve: {sketch id, "g<entity id>"}
  // Connector only: snapElement(2) for an edge/corner snap, innerPath inside
  // a sub-assembly. jobPartId: which job part a pick on a CAM job tab hit.
};
```

`items` is ordered because pick order matters to tools. `SelectionKindBit(k)`
is the bit a kind occupies in every filter mask (`SlotSpec::kinds`,
`SlotPickState::kinds`, the pipeline's candidate filter).

Renderer highlights are a **projection** of this set, rebuilt each frame in
one place (`SyncSelectionHighlights`), not parallel state held by the
renderers. Multiple selected faces on one mesh all show (per-instance range
lists). The sketch editor keeps its own entity selection
(`SketchEditState`), which follows the same conventions and is load-bearing
for drag sessions and constraint argument order; the two meet at the tool
session and prompt level ([tool-sessions.md](tool-sessions.md)).

`get_state` reports the set as `selection_items` (`[{kind, id, element}]`).
`select_face`, `select_feature`, `select_occurrence` and `select_mate` drive
it.

## The pick pipeline

`HandleSelectionInput` (`src/ui/SelectionPipeline.{h,cpp}`) builds the pick
ray once per frame and asks `Candidates()` for everything under the cursor
that the current filter accepts, ranked (edge before face at the same spot,
then body):

- **Filter.** With nothing armed, a part tab accepts Edge | Face | Body and
  an assembly tab accepts Occurrence (a face hit selects the whole
  occurrence). An armed context — a tool's active pick slot, measure, a
  simulation boundary-condition tool, a mate connector — installs its own
  kinds and prompt, and the resolved pick goes to that tool instead of the
  selection (`ArmedPrompt` feeds the prompt bar).
- **Hover.** The top candidate highlights before any click. On an assembly
  the whole hovered occurrence lights (every mesh of it, glass included), not
  the face under the cursor.
- **Click.** A plain click replaces the selection; empty space clears it
  (on an assembly, only the occurrence kind — a selected mate stays).
  **Ctrl+click toggles** (multi-select). **Clicking the same spot again
  cycles** through the stacked candidates: edge → face → body. Shift+click
  also aligns the camera to the picked face.
- **Escape** clears an armed filter first, then the selection.

`Kind::SketchCurve` is a job part's sketch entity drawn on a job tab, armed
only by the Engrave verb: the nearest drawn curve on screen, not one the part
hides ([cam/gui.md](../cam/gui.md#engrave-from-the-viewport)).

`Candidates()` never emits `Kind::Feature`: a sketch or a feature is picked
from the feature tree, not the viewport. A tool that needs a sketch resolves
it the way `CreateRib` does (the selected sketch, else the newest with a
closed profile).

`FaceSelector` (`src/cad/FaceSelector.{h,cpp}`) is the pick engine (ray
queries against the registered render meshes); it does not own selection
state. `RendererSync` is the one place that registers meshes with it, so the
picker cannot disagree with what was drawn.

The feature tree marks the feature that made a picked face
(`FeaturesThatMadeThePick`) with a dot in the highlight colour — marked, not
selected, so Delete stays unambiguous — and scrolls a part picked in the
viewport into view once per selection change, only if it is out of sight.

## Picking through a section

A section is a fragment discard in every render pass, so the picker applies
the same planes. `SyncDocumentRenderers` pushes the enabled planes to
`FaceSelector::SetClipPlanes` every frame, ahead of its version and
cap-rebuild gates (a section being dragged does not rebuild caps, but must
still clip picks). `FaceSelector.h` takes a span of planes and stays free of
`render/`.

- **Nothing on the removed side is pickable.** `PickFace` computes the kept
  interval `[tEnter, tExit]` along the ray once per pick; a triangle hit is
  visible exactly when `tEnter <= t <= tExit`, two float compares per
  triangle in a loop that runs every hovered frame. `PickEdge` tests the
  closest point on a candidate segment against the planes, after the
  pick-radius reject.
- **The cut face is the body it cuts.** While scanning a mesh, `PickFace`
  remembers the last boundary crossed before `tEnter` and whether it was
  front-facing: entering last means the ray was inside that solid at the
  plane, so what is visible there is its section cap — a hit at `tEnter` with
  the plane's normal and `RayHit::cap = true`. (Orientation of the last
  crossing, not crossing parity, because a ray through the shared edge of two
  faces reports two hits at one `t`.) This needs a closed solid:
  `AddMesh(..., solid)` comes from `BodyTessellation::solid`
  (`CADShape::SolidCount()`), and an open shell gets no inferred cap.
- **A cap names no element.** It is not a B-Rep face, so a cap hit produces
  Body and Occurrence candidates and never a Face, and every site that turns
  a hit into a named element refuses it: the Face branch of `Candidates`,
  `PickAssemblyFace` (mate connectors), the sketch-plane pick and the Project
  tool. It is never silently replaced by the face the ray entered through.
- **A cap lights with its body.** `AppState::gpu.sectionCapMeshes` records each
  cap's `occurrenceId` and `bodyIndex`, and the hover/selection projection
  lights caps with the rest of their occurrence or body.

The cap meshes themselves are not registered with the picker, so picking one
cap of several overlapping bodies at the same point, or naming a cut face as
an element, is not possible.

Tests: `tests/test_faceselector.cpp` ("Picking through a section view": a cap
from the cut side, nothing changed from the other side, a pick through a
removed corner, planes disabled reproducing the unclipped answers, edges
behind the cut, `solid = false`); GUI `section/a_cut_picks_what_you_can_see`.

## Lists: one gesture, one Delete

Features in a part, occurrences, mates and relations in an assembly, and
every row of a CAM job -- operations, setups, parts, fixtures and tools --
share the gesture and the verb
(`src/ui/ListSelection.{h,cpp}`), not the row widget:

- `ListRowClicked(state, kind, id, order, ownKindOnly, clickClears)`: a plain
  click selects the row alone and becomes the range anchor
  (`Selection::rangeAnchor`, per kind); Ctrl toggles it; Shift selects from
  the anchor to the row over `order` (the rows as the panel lists them),
  replacing that kind; Ctrl+Shift adds the range. `ownKindOnly` makes a plain
  click clear only its own kind (an occurrence click keeps the selected
  mate).
- `clickClears` (the mate list): a plain click on the row that is the whole
  selection of its kind clears it — the way to put a selection down. Off
  elsewhere, because most lists select what they just created, and clicking
  that row to confirm it must not deselect it.
- `ListRowContext`: a right-click inside the selection keeps it; outside, the
  row becomes the selection. Menu verbs count what they act on
  (`DeleteLabel`: "Delete 3 features"; `RowActionLabel`: "Hide 3
  occurrences").
- A right **click** on an occurrence in the viewport is the same gesture
  (`HandleSelectionInput`, on an assembly tab with nothing armed): the top
  candidate under the cursor takes `ListRowContext`'s rule and its menu
  opens. Only a release short of ImGui's drag threshold counts — a right drag
  navigates (orbits, by default), and moves the camera only once past that
  threshold, so a click never nudges the view, whatever preset is chosen. Empty space opens nothing.
- `DeleteSelection`: the one Delete, called by the Delete key (from the
  modal-key chain in `ViewportInput.cpp`, which yields to armed slots, a mate
  or fastener session, a text field, an edited sketch and an open editor) and
  by every list's menu. Features go through `RemoveFeaturesCommand` (one undo
  step, one regeneration); on an assembly, mates and occurrences go together
  in one snapshot (`DeleteSelectedOccurrences(state, takeSnapshot=false)`);
  on a job, operations, tools, fixtures, parts and setups, in that order, in
  one undo step (`RemoveCamOperations`, `RemoveCamTools`, `RemoveCamFixtures`,
  `RemoveJobParts`, `RemoveCamSetups`, each also a step of its own when
  called alone). A CAM row menu's Delete takes only its own kind, since it
  names it; a plain click on a CAM row lets go of the panel's other row kinds
  (`ui::CamRowClicked`), so a setup clicked a moment ago does not go with the
  tool Delete was pressed for. A tool selection is the active setup's, and
  goes when another setup is made active.

A range delete that leaves a dependent broken (an extrude whose sketch went)
does so, and the row turns red with the reason, as a single delete always has.
Delete does not yet reach relations.

GUI: `selection/survives_sibling_delete`,
`assembly/shift_click_selects_the_rows_between`,
`assembly/a_mate_row_closes_and_deletes`,
`cam/operations_select_like_a_list_and_delete_together`,
`cam/delete_reaches_every_row_kind`.

## Not built

Depth cycling to a face hidden behind the first (the engine returns the
nearest face per ray, so cycling reaches edge, face and body only); box or
lasso selection in 3D; a selection model on drawing sheets (a drawing row's
click highlights nothing on the sheet).
