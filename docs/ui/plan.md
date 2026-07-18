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

**Partly shipped** (`a122590`): toolbar background bar, viewport-favoring
default split, untracked stale `imgui.ini`, "Reset Layout" menu item,
`select_feature` RPC. The rest, now including the audit's panel triage:

- **Every panel gets a persistent dock node.** Configurations, Study,
  Occurrence, Drawing panels join the layout (right dock, tabbed with
  Properties, or bottom — decide per panel). Fixes the floating-Configurations
  bug and the deferred sketch-panel issue: Properties and Sketch in
  **separate, persistent** dock nodes so gating one window's submission can't
  garbage-collect the other's home.
- **Properties layout**: labels-left / two-column so nothing clips at panel
  widths; minimum sizes/constraints on panels and dialogs; audit fixed pixel
  constants (screen-space tolerances DPI-scaled).
- **Controls help out of the panel** — a `?` overlay or Help menu entry, not
  permanent panel real estate.
- **DPI awareness**: scale fonts + `ImGuiStyle` by monitor content scale
  (GLFW `glfwGetWindowContentScale`), rescale on monitor change; user
  font-size setting.
- **Toolbars overflow** (wrap or scroll) instead of clipping when the window
  is narrower than the button row (`SketchToolbar`, main `Toolbar`).

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

Feature creation/editing as a consistent small form (the Onshape dialog):
named **pick slots** ("Profiles", "Axis", "Mirror plane") with the active
slot highlighted and driving U1's filter — clicking in the viewport fills the
slot; the existing ref combos remain as the alternate/precision path. Numeric
fields accept expressions (V2), mode toggles, live preview via
`PreviewFeature`, ✓/✗ accept. Build it once as a reusable frame (extending
`FeatureEditor` / `PropertiesPanel`) and every feature's UI converges on it —
this answers "how do I choose which features a transform applies to": the
dialog's pick slots + the feature tree as an alternate pick surface.

## U2 — Sketch-region selection for extrude/revolve

The user-pain: extrude takes the whole sketch — you can't pick *which closed
region* to extrude. `SketchProfile::BuildLoops` already computes the loops;
make them first-class:

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

Make it look like a product, not a debug build: spacing/padding rhythm and
consistent icon sizing in `Theme`/`Toolbar`; hierarchy in the feature tree
(icons per feature kind, error/suppressed states, bodies styled as objects
not bullet-debug lines); viewport chrome (axis triad + named-view menu —
`Shift+1..7` exists, give it UI; view cube only if the triad disappoints);
empty states ("No sketch — pick a plane to start"); consistent tooltips
(name + shortcut) on every control.

## U6 — Command palette + discoverability

`Ctrl+K` fuzzy palette over every command (tools, constraints, features,
views) with shortcuts shown — cheap because the command layer already
enumerates the surface, and it doubles as living documentation of goal 5
("the LLM and the human use the same commands" — so does the palette).

## Tests

GUI-test the behaviors, eyeball the aesthetics: every panel docked after
"Reset Layout" + no window overlaps the viewport rect (U4, gui_smoke);
`region/pick_hole_loop` (U2: hover shades, click selects, extrude of one
region of a two-region sketch); dialog slot-picking flows (U3); selection
filter + disambiguation cycling (U1); toolbar overflow at narrow width (U4);
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
