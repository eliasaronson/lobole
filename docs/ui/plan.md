# UI & UX — plan

The workbench shell shipped (docking layout, dark theme, icon toolbar,
preview→accept, default planes, origin gizmo — roadmap "CAD UI"). What remains
is the difference between "has a UI" and "is pleasant": a real selection model,
picking *what* to operate on (which profile to extrude, which features to
pattern), layout that fits its contents, and a visual-polish pass.

Milestones are small and independently shippable — interleave them with the
modeling work they serve (U2/U3 directly unblock modeling M1–M5 UX).

## U1 — Unified selection model

One selection concept across contexts instead of per-tool ad-hoc state:
a typed selection set (sketch entity / face / edge / body / feature /
sketch-region) with:
- **Type filters** — a tool declares what it accepts (Mirror: bodies +
  features; Extrude: regions/faces); picking auto-filters, the status bar says
  what's expected (extends the sketcher's staged prompt bar, Phase N1, to
  model-space tools).
- **Hover preview** everywhere — what a click would select highlights first
  (the sketcher has this; model space only has it for faces).
- **Disambiguation** — repeated click or a small "select other" popup cycles
  overlapping candidates (face behind face, edge vs face).
Selected-feature highlight (NF2) and the `FaceSelector` are the seeds; this
generalizes them. Lives in `AppState` + `ViewportInput`.

## U2 — Sketch-region selection for extrude/revolve

The user-pain: extrude currently takes the whole sketch — you can't pick *which
closed region* to extrude. `SketchProfile::BuildLoops` already computes the
loops; make them first-class:
- Hit-test regions (point-in-loop, respecting holes), shade the hovered
  region, click to toggle — multi-region select (Onshape's shaded-profile
  pick).
- The extrude/revolve feature stores **selected loop refs** (keyed by the
  boundary entities' stable ids so the choice survives sketch edits;
  a vanished loop → feature error state, per the modeling regen-failure UX).
- Default remains all regions, so existing files and the quick path are
  unchanged.

## U3 — Feature-dialog pattern

Feature creation/editing as a consistent small form (the Onshape dialog):
named **pick slots** ("Profiles", "Axis", "Mirror plane") with the active slot
highlighted and driving U1's filter, numeric fields that accept expressions
(variables V2), mode toggles, live preview via `PreviewFeature`, ✓/✗ accept.
Build it once as a reusable frame (extending `FeatureEditor` /
`PropertiesPanel`) and every new modeling feature (datum plane, revolve,
boolean, pattern) gets its UI nearly for free — this answers "how do I choose
which features a transform applies to": the dialog's pick slots + the feature
tree as an alternate pick surface.

## U4 — Layout & scaling correctness

The "windows don't fit their buttons" class of bugs:
- **DPI awareness**: scale fonts + `ImGuiStyle` by monitor content scale
  (GLFW `glfwGetWindowContentScale`), rescale on monitor change; a user font-
  size setting.
- **Toolbars overflow** instead of clipping: wrap or scroll when the window is
  narrower than the button row (`SketchToolbar`, main `Toolbar`).
- **Minimum sizes / constraints** on panels and dialogs so nothing truncates;
  audit every fixed pixel constant (sizes, paddings, pick tolerances already
  zoom-scaled in-sketch — screen-space ones must be DPI-scaled).
- Persisted layout sanity: a "reset layout" action (`DockLayout`).

## U5 — Visual design pass

Make it look like a product, not a debug build: spacing/padding rhythm and
consistent icon sizing in `Theme`/`Toolbar`; hierarchy in the feature tree
(icons per feature kind, error/suppressed states from the modeling
regen-failure UX); viewport chrome (view cube or axis triad + named-view
menu — `Shift+1..7` exists, give it UI); empty states ("No sketch — pick a
plane to start"); consistent tooltips (name + shortcut) on every control.
Pairs with rendering 6.1–6.4 — AA + tone mapping do as much for perceived
quality as any widget work.

## U6 — Command palette + discoverability

`Ctrl+K` fuzzy palette over every command (tools, constraints, features,
views) with shortcuts shown — cheap because the command layer already
enumerates the surface, and it doubles as living documentation of goal 5
("the LLM and the human use the same commands" — so does the palette).

## Tests

GUI-test the behaviors, eyeball the aesthetics: `region/pick_hole_loop`
(U2: hover shades, click selects, extrude of one region of a two-region
sketch), dialog slot-picking flows (U3), toolbar overflow at narrow width
(U4), palette executes a command (U6).

## Recommended order

U2 → U3 with modeling M1–M2 (they're that work's UX half) → U4 (small,
irritating, do soon) → U1 progressively as U2/U3 demand it → U5 → U6.
