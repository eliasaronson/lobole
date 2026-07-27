# UI & UX — plan

The workbench shell shipped (docking layout, dark theme, icon toolbar,
preview→accept, default planes, origin gizmo — roadmap "CAD UI"). What remains
is the difference between "has a UI" and "is pleasant": a real selection model,
picking *what* to operate on (which profile to extrude, which features to
pattern), layout that fits its contents, and a visual-polish pass.

> **Revised 2026-07-18** after a live audit (screenshots via dev-watch + code
> read). The original order (U2 first, interleaved with modeling M1–M2) is
> moot — modeling M1–M8 shipped. The re-derived order is below; the audit
> findings drive it.

## Audit findings (2026-07-18)

Observed on the running app (empty project + `v17-full` golden):

1. **Panels without dock homes float over the viewport.** The Configurations
   panel renders dead-center on top of the model. Every panel added after the
   workbench shipped (Configurations, Study, Occurrence, Drawing) improvised
   its own window instead of joining the dock layout — the same class of bug
   as the deferred "shared right dock node" problem from the first U4 pass.
2. **The right Properties panel clips every line** ("Select one in the Model
   pa…", "Left click: select fac…"): labels-after-fields + no minimum width.
   A static "Controls" help block permanently occupies panel space.
3. **The feature tree is a flat text list** — no icons, no error/suppressed
   visual states, bodies as debug-style bullet lines with volumes.
4. **Selection is six ad-hoc mechanisms** (`AppState.h`): `selectedFeature`
   (a raw *index*, not a stable id), `selectedOccurrence`, `selectedMate`,
   the sketch `selected` point list, `FaceSelector` (hover + click, faces
   only), `EdgePickState` (measure only). No shared filters, no shared hover
   preview, no disambiguation.
5. **Feature references are combo dropdowns, not viewport picks**
   (`FeatureEditor.cpp`: `DrawAxisRefCombo`, `PlaneRefCombo`,
   `SourceBodyCombo`). You cannot click a face to fill "Mirror plane".
6. **U2 is unshipped**: no feature stores selected-loop refs; extrude/revolve
   still take the whole sketch.

## U4 — Layout & docking correctness

**Shipped 2026-07-18** (this pass; earlier: `a122590` toolbar background bar,
viewport-favoring default split, untracked stale `imgui.ini`, "Reset Layout",
`select_feature` RPC):

- **Dock-home enforcement** (`DockLayout.cpp`): once per launch, after the
  panel windows exist, panels a stale `imgui.ini` left floating are re-docked
  — secondary panels (Variables, Configurations, Sketch) join their sibling's
  node; a floating *core* panel (Model/Properties) triggers the full default
  rebuild. Fixes the floating-Configurations bug for good: `DockBuilder`
  entries only apply on rebuild, so any panel added after a user's ini was
  saved floated forever. (Study/Occurrence/Drawing reuse the "Model" window,
  so they were never affected.)
- **Labels-left rows** (`FeatureEditor.cpp` `LeftLabel`): label column left,
  field filling the row; stacks label-above-field when the panel is too
  narrow for two usable columns; fixed two-column in auto-resize windows (the
  preview dialog — avail-based widths are circular there). All feature
  combos/drags/expression fields converted; checkboxes stay label-right.
- **Controls help → View > Viewport Controls** overlay (dismissable,
  viewport bottom-left); the static block no longer eats Properties space.
- **`WindowMinSize` floor** (180×60, `Theme.cpp`) — applies to dock-node
  resize limits too, so panels can't be dragged to slivers.
- **Toolbar overflow → horizontal scroll** (`DockLayout.cpp` toolbar row;
  the row grows by the scrollbar height while overflowed). The sketch tool
  icons live in the same top row, so both bars are covered.
- **DPI**: startup content-scale theme/font scaling existed; added rescale on
  monitor change (GLFW content-scale callback → re-apply theme + rebuild the
  Vulkan font atlas between frames).

Shipped alongside (found while verifying): a pre-existing
`FaceSelector::PickEdge` bug — the best candidate was chosen by angular
closeness alone, so an *occluded* edge collinear with the ray (a box-diagonal
view lines up both corners) could win and then be rejected by the
whole-result occlusion filter, returning "no hit" over a visible edge; the
occlusion check now filters per candidate. Also `tools/gui-tests-xvfb.sh`:
the GUI suite on a virtual display — immune to the session-lock present
throttling that made `ctest -R gui_smoke` time out (and the seed for the CI
virtual-display item).

Still open (folded into U5): user font-size setting; audit remaining
screen-space pixel constants; a narrow-width toolbar GUI test.

## U1 — Unified selection model

One selection concept across contexts instead of the six mechanisms above:
a typed selection set (sketch entity / face / edge / body / feature /
occurrence / mate / sketch-region) living in `AppState` + `ViewportInput`:

- **Stable-id entries** — `{kind, document element id, topo ref}` variants,
  replacing `selectedFeature`'s raw index (a known dangling-index hazard
  across regenerations).
- **Type filters** — a tool declares what it accepts (Mirror: bodies +
  features; Extrude: regions/faces); picking auto-filters, the status bar
  says what's expected (extends the sketcher's staged prompt bar to
  model-space tools).
- **Hover preview everywhere** — what a click would select highlights first
  (faces have it via `FaceSelector`; extend to edges, bodies, occurrences).
- **Disambiguation** — repeated click or a small "select other" popup cycles
  overlapping candidates (face behind face, edge vs face).
- **Migration, not parallel state**: `FaceSelector`, `EdgePickState`, and the
  three id fields become views onto / feeders into the one set, so panels and
  RPCs (`get_state` selection block, `select_face`, `select_feature`) read
  one source of truth.

## U3 — Feature-dialog pattern

**SHIPPED 2026-07-18** (pick slots): PickSlotRow + SlotPickState armed
context; wired for extrude up-to-face (replacing the all-faces combo),
boolean target/tool, pattern source, shell body/open-faces, draft faces,
and U2's Profiles regions. Combos remain the precision path. Remaining from
the original sketch (folded into U5/U6 polish): a full dialog *frame*
beyond the slots — the current dialogs already share LeftLabel/slot rows.
Original design:
named **pick slots** ("Profiles", "Axis", "Mirror plane") with the active
slot highlighted and driving U1's filter — clicking in the viewport fills the
slot; the existing ref combos remain as the alternate/precision path. Numeric
fields accept expressions (V2), mode toggles, live preview via
`PreviewFeature`, ✓/✗ accept. Build it once as a reusable frame (extending
`FeatureEditor` / `PropertiesPanel`) and every feature's UI converges on it —
this answers "how do I choose which features a transform applies to": the
dialog's pick slots + the feature tree as an alternate pick surface.

## U2 — Sketch-region selection for extrude/revolve

**SHIPPED 2026-07-18** (format v34): regions stored as outer-loop entity-id
sets (empty = whole sketch, back-compat); regen filters BuildLoops with
holes riding along; vanished region = regen error with re-pick guidance;
picked in the viewport via the U3 Profiles slot (even-odd containment;
hover/selected loop boundaries emphasized). Deferred: shaded region fill
(boundary emphasis shipped instead — shading needs face triangulation).
Original design:

- Hit-test regions (point-in-loop, respecting holes), shade the hovered
  region, click to toggle — multi-region select (Onshape's shaded-profile
  pick). Region becomes a U1 selection kind; the extrude/revolve dialog's
  "Profiles" slot (U3) is where it lands.
- The feature stores **selected loop refs** keyed by the boundary entities'
  stable ids so the choice survives sketch edits; a vanished loop → feature
  error state, per the modeling regen-failure UX.
- Default remains all regions, so existing files and the quick path are
  unchanged.

## U5 — Visual design pass

**Core items SHIPPED 2026-07-18**: feature-tree per-kind icons + styled
bodies rows; axis triad + View > Standard Views; empty-state hint;
View > Font Size (session, via the DPI rebuild path); multi-face highlight
projection (per-instance; same-body faces still last-wins). Remaining:
spacing/padding rhythm audit, error/suppressed tree BADGES beyond color,
shaded region fill, whole-instance occurrence hover, tooltips audit.
Original scope:
Make it look like a product, not a debug build: spacing/padding rhythm and
consistent icon sizing in `Theme`/`Toolbar`; hierarchy in the feature tree
(icons per feature kind, error/suppressed states, bodies styled as objects
not bullet-debug lines); viewport chrome (axis triad + named-view menu —
`Shift+1..7` exists, give it UI; view cube only if the triad disappoints);
empty states ("No sketch — pick a plane to start"); consistent tooltips
(name + shortcut) on every control.

## U6 — Command palette + discoverability

**SHIPPED 2026-07-18** (`ui/CommandPalette.cpp`): Ctrl+K fuzzy palette over
the model-context surface (create/tools/edit/view/file), menu gating
mirrored, shortcuts shown, same action functions as the menus. Not yet
enumerated: sketch-mode tools and constraint commands (the sketch keymap
owns those contexts) — extend when the sketch toolbar consolidates.

## U7 — Tool sessions & right-panel removal

**SHIPPED 2026-07-27** (U7a–U7e, commits in the design doc's status line) —
[u7-tool-sessions.md](u7-tool-sessions.md).
One pattern for every pick-collecting tool: declarative slot specs rendered
in a single floating session window (top-right anchor; the bottom-center
preview dialog and its click-occlusion class die), exactly one armed slot
driving U1's filter, per-item editing everywhere, greedy seed from
pre-selection (select-first like SW/Onshape), mate connectors folded into
`SelectionItem` (`Kind::Connector` — the last private picker deletes), and
the right Properties panel **removed entirely** (feature editing becomes an
explicit session; mass props → Model panel; legacy readouts die; Sketch
options re-home left). Phases U7a–U7e in the design doc.

## U8 — Sketch sessions

**SHIPPED 2026-07-27** — [u8-sketch-sessions.md](u8-sketch-sessions.md).
Sketch tool sessions (Mirror/Fillet/Pattern/Offset/Calibrate/Dimension +
plane pick) in the floating Tool window; the Sketch panel is DELETED (its
tenants: toolbar toggles, Model-panel sections, toolbar Extrude/Revolve);
one shared prompt-pill renderer; sketch selection seeds extrude/revolve
regions (select-profile-first). The sketch entity-selection store stays
internal by design (U8c full-merge deliberately not done). Dockspace id
versioned (one-time layout reset) so stale inis can't resurrect dead nodes.

## Post-U8 polish rounds (all shipped 2026-07-27)

Dogfooding-driven follow-ups, one round per user report; commits inline.

- **Interaction grammar** (`fee678c`, `3dec193`): three sketch tool
  families — collect tools (Mirror/Pattern/Offset: click toggles, drag
  box-selects, tap-empty/Enter confirms), act-per-click tools (Fillet/
  Trim: tap-empty/Enter finishes), draw tools (chain until Esc). Apply
  returns to Select; a preselected sole construction line IS the mirror
  axis (one-gesture mirror); the Tool window clamps inside the app window;
  the Z zoom key died (collided with undo/redo).
- **Ops as first-class objects** (`f1fe8be`, `508649b`…`905daef`,
  `ead9cf3`): Operations list in the Model panel (double-click → edit
  session); reference images are a tool session; **format v50** 2D linear
  patterns (row-major k, back-compat); **format v51** mirror as a derived
  op (tracks sources AND axis; symmetry-constraint fan-out deleted) +
  circular-pattern centre REFERENCES (Pick button; tracks the point); op
  edit sessions re-pick sources/axis; trim guards generated instances.
- **Input & feedback** (`3c22fcc`, `18d0726`, `ab1d45c`, `6053d96`):
  multi-select face highlight (per-instance range lists + discard-outside
  sub-draws — U1e projection completeness); constraint badges group per
  entity, real-width layout, ">2 collapses to xN + hover-fan";
  point-on-curve snaps combine with one axis alignment (top-of-circle);
  trim splits circles crossed by circles; view cube above the tab bar;
  **ui/ExprInput** = the one numeric-input widget family — every wrapped
  field evaluates expressions/variables on commit ("360/3", "width/2");
  stored-expression fields keep ExprDim (persistence ⇒ re-evaluation).

State after this: one selection pipeline, one floating Tool window, one
prompt bar, one collect-phase grammar, and one numeric-input widget across
model, assembly, drawing, and sketch spaces. Remaining leftovers live in
the backlog below (notably: ExprInput for the DragFloat3 fields, curve
trim previews, U1e depth cycling, U8c store merge if ever).

## Backlog — standard-CAD affordances not yet scheduled

Collected 2026-07-18 (user ask: "what are we missing vs other CAD?").
Feature-tree context menu (rename/suppress/delete/rollback/re-pick) and the
draggable rollback bar shipped with U4. Shipped 2026-07-19 (small-wins
batch): **hide/show bodies** (glyph = eye toggle; context menu kept),
**double-click a feature row** → Properties editor (sketch/datum keep their
special double-clicks), **zoom to selection** (Shift+F / View menu /
palette; occurrence-aware via Measure::ElementBounds), **window title =
project name + dirty marker** (all tabs' saved versions stamped on save —
assemblies/stashed tabs read clean now). Still missing, roughly by value:

- **Direct manipulation in previews — shipped** (`9479582`): drag handles set
  pending extrude distance, fillet/chamfer radius, or revolve angle. Missed
  handle clicks now fall through to selection and blend-edge picking; a
  revolve-handle GUI case remains. See
  [direct-manipulation.md](direct-manipulation.md).
- **Feature reordering by drag in the tree** (with a dependency check and a
  clear failure message). Standard in SW/FreeCAD; needs care with refs.
- **Per-body appearance/material from the tree** (right-click > Material) —
  material exists on features; surface it where users look for it.
- **Quick toolbar at cursor** (SolidWorks "S key") — the mouse-side sibling
  of U6's Ctrl+K palette; same command enumeration feeds both.
- **Dimension-first sketching** — type the length/radius while drawing
  (Onshape). Sketcher-side; bigger, belongs to a sketching follow-on.
- **Unit display/entry in dimension fields** (deferred from variables C2).

## Tests

GUI-test the behaviors, eyeball the aesthetics: every panel docked after
"Reset Layout" + no window overlaps the viewport rect (U4, gui_smoke);
`region/pick_hole_loop` (U2: hover shades, click selects, extrude of one
region of a two-region sketch); dialog slot-picking flows (U3); selection
filter + disambiguation cycling including stacked-face depth cycling (U1e);
toolbar overflow at narrow width (U4);
palette executes a command (U6). Visuals verified via dev-watch screenshots
at 1080p and 4K-scaled.

## Recommended order (revised 2026-07-18)

**U4 → U1 → U3 → U2 → U5 → U6.**

- **U4 first**: small, mechanical, and it's the daily irritation — the app
  currently renders a panel *on top of the model*. Also the cheapest to
  verify (screenshot-diff via dev-watch).
- **U1 before U3/U2**: both consume the selection model's filters; building
  dialog slots or region picking on the six ad-hoc mechanisms would just add
  a seventh.
- **U3 before U2**: the region pick lands inside the extrude dialog's
  "Profiles" slot, so the slot frame must exist. U2 is the headline
  user-facing win of the workstream.
- **U5 then U6** close it out; both are independent and can interleave
  anywhere if a release needs to look good sooner.
- **U7 (added 2026-07-27)** follows the v1 workstream: it consumes U1's
  filters and U3's slots and supersedes the "full dialog frame" remnant of
  U3 — see [u7-tool-sessions.md](u7-tool-sessions.md).
