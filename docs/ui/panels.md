# Panels and floating windows

The app's patterns, which every panel follows:

- **A docked panel holds the document's structure** — lists of the things
  the document is made of, read, selected from and edited in place.
- **A list is rows** in the feature list's style: a type glyph, the name,
  a dim tail; click selects (Ctrl/Shift extend, [selection.md](selection.md#lists-one-gesture-one-delete)),
  double-click edits, right-click offers the rest.
- **Editing happens in the floating tool window**, not inline under the list,
  so the list does not change length under the cursor
  ([tool-sessions.md](tool-sessions.md)).
- **A viewport mode floats.** Something driven in the viewport while you
  watch the model (exploded views, motion, simulation) is a tool window
  opened from the toolbar, not a docked section.
- **A verb with a result is a toolbar button that opens a window** (Generate,
  the BOM), and long work shows the shared progress bar
  ([long-operations.md](long-operations.md)).
- **Words.** A window that creates something says **Accept** / **Cancel**
  (`###tw_accept` / `###tw_cancel`); a window that applies as you go says
  **Close**. One trash glyph (`ICON_FA_TRASH_CAN`). One glyph per command,
  the same on every toolbar and in every list ([README.md](README.md#icons)).

## Shared widgets

- **`ui::ListRow`** (`src/ui/Widgets.h`): glyph, label, tail, dimmed state,
  a tail glyph, and an eye that appears on hover (always shown, struck
  through, when the row is hidden). It elides a name that does not fit and
  gives it back on hover.
- **`ui::SectionHeader(label, count, defaultOpen)`**: the one disclosure
  header — a chevron that points right when closed and down when open, an
  optional count ("Occurrences 22"), a hover highlight across the row, drawn
  as a row rather than a filled bar. Its open state is stored as
  `CollapsingHeader`'s was. There is no `ImGui::CollapsingHeader` left in
  `src/ui`.
- **`TextFit.h`** (`TextEllipsis`, `SelectableEllipsis`, …): text that might
  not fit is elided with `…` and the full string appears in a tooltip, using
  ImGui's own `RenderTextEllipsis`. Text never clips silently.
- **`ui::WrapRow`** (`src/ui/PanelLayout.h`): call `Next(width)` before each
  widget of a row and it breaks the line when the next one will not fit.
  `SameLine()` never wraps; a button past the panel edge cannot be seen or
  clicked.
- **Tables adapt, never clip digits.** A table cell clips rather than elides,
  and a number that loses its trailing digits still looks like a number. The
  Variables table drops to two rows per item when narrow; the BOM is a
  floating window so its columns get the width they need.

## Floating windows

`ui::BeginFloatingWindow` (`src/ui/FloatingWindow.{h,cpp}`) places every
floating window the same way: anchored to a viewport corner (`TopRight`,
`BelowToolWindow`, `BottomCenter`), re-anchored when the viewport moves or the
window reappears, and kept on that corner as its size changes until it is
dragged. It is sized afresh when an auto-resized one reappears, its child
windows with it (the tool window is one ImGui window shared by every
session, and its right-aligned rows and full-width lists kept the previous
session's width, so a Thread window opened after a wide Simulation window
covered the viewport it asks you to click in). It is clamped inside the app
every frame **before** `Begin`, from the size `Begin` is about to give it:
moved after `Begin`, the window's content is still laid out where the window
was, so a list as wide as the window reaches past its edge by the move, the
auto-resize adopts that as its width, and the CAM window grew by the clamp's
step every frame (`cam/tool_window_keeps_its_own_width`). It has the elevated
border and a drawn shadow (`DrawWindowShadow`; ImGui has no shadow
primitive), and `NoSavedSettings`. A window anchored `BelowToolWindow` stacks
under the tool window (`state.toolWindowRect`) instead of opening on top of
it. Users: the tool window itself, the Generate window, the BOM window and
the CAM playback transport (bottom-centre). Every one but the tool window
records its rect as an obstacle Fit keeps the model out from under
([README.md](README.md#viewport)).

## Part tab: the Model panel

The feature tree (`src/ui/FeatureTreePanel.cpp`):

- One row per feature with its command's glyph (`FeatureIcon`, from the one
  icon table), and a badge glyph per state — failed, warned, suppressed,
  rolled past (`FeatureRowBadge`) — because two of those are dimmed and were
  otherwise indistinguishable. A failed row is red with the reason.
- The base planes and datum planes are rows too: a click fills an armed
  plane pick, a double-click starts a sketch on that plane. A datum
  plane's or a sketch's row also fills Replace Face's armed target.
- Double-click opens the feature's editor in the tool window (a sketch's
  double-click enters the sketch). Right-click: Edit, Rename, Suppress,
  Rollback to here / Roll to end, the derived-part verbs, re-pick verbs,
  Delete (counted).
- Drag a row onto another to reorder, validated on drop: a move that would
  dangle a reference is refused with a message.
- The rollback bar (`##RollbackBar`) is a draggable divider where
  regeneration stops.
- Body rows: an eye toggles visibility; the context menu has Hide/Show,
  Material and Appearance.
- Sections below: Mass properties (with a centre-of-gravity marker),
  and, while a sketch is edited, its Operations and Constraints.

## Assembly tab: the Model panel

`src/ui/OccurrencePanel.cpp`. The panel lists what the assembly **is**:

- **Occurrences** — a glyph (part or sub-assembly), the name, an anchor
  tail glyph when fixed, dimmed when hidden, the hover eye for visibility.
  A [flexible](../assemblies/README.md#flexible-sub-assemblies) placement
  reads "flexible" in its tail and expands (`###occ_flex_<id>`, open by
  default) to its moving parts (`###occ_in_<placement>_<part>`, the mate
  type that moves each as the tail) and the mates inside it that turn or
  slide (`###occ_inmate_<placement>_<mate>`, the value this placement has,
  "held" when its configuration holds it). They are read-only: a part row
  selects that part, a mate row the part it moves, and the part then drags
  in the viewport. The rest of the unit folds into its own row.
  Right-click: Edit, Open part (or assembly), Hide/Show, Detach from its
  part (a rider), Delete. **The same menu opens on the part in the
  viewport** (`DrawViewportOccurrenceMenu`, headed with its name): both draw
  `DrawOccurrenceMenu` and apply it with `ApplyOccurrenceMenu`, so the two
  cannot offer different verbs.
- **Mates** — a glyph per type, then the mate's name or "A – B" (the two
  occurrences it joins), the type as a dim tail; red with the reason in the
  tooltip when it failed to solve. The mate list uses `clickClears`.
- **Relations**.

Editors float. A double-click (or Edit) opens an occurrence's or a relation's
properties in `DrawAssemblyEditWindow`, and a mate's (name, flip, rotate, its
DOF fields — `DrawMateDofFields`) in the mate tool's edit mode
(`DrawMateToolWindow`). Each field edit is one undo step per drag.

- A part placement's editor adds its **Configuration** picker
  (`###occ_part_config`, shown when the part has configurations) and its
  **Material** (`###occ_material`: "The part's own" or a slug), through
  `ui::SetOccurrenceConfiguration` / `ui::SetOccurrenceMaterial`, one undo
  step each ([per placement](../assemblies/README.md#configurations-per-placement)).
- A sub-assembly placement's editor adds its **Configuration** picker
  (`###occ_config`: "Its active one (…)" or one of the sub-assembly's
  configurations) and **Flexible** (`###occ_flexible`). Both go through
  `ui::SetOccurrenceConfiguration` / `ui::SetOccurrenceFlexible`
  (`src/ui/FlexibleActions.h`), the same actions the RPC calls, one undo step
  each. A refusal (nothing fixed inside, a freewheel or click, a second level
  of flexibility) is shown in red under the controls
  (`AppState::asmEditRefusal`) and changes nothing.
- The relation editor has a **Drives** and a **Driven** combo
  (`###rel_side_a`, `###rel_side_b`) listing every mate that turns or slides:
  this assembly's own, then each flexible placement's as "Pump / camshaft"
  (`ui::DofMateChoices`). A mate lacking the DOF the side reads is refused in
  place; a screw's one mate is both sides. A gear made from teeth edits its
  teeth (`###rel_teeth_a`, `###rel_teeth_b`) and shows the ratio; a relation
  over several mates shows a combo per member by role (`###rel_term_<i>`),
  its teeth and its constant (`###rel_constant`); a contact relation shows
  its sense (`###rel_toward_edit`) and its table, with no ratio or phase.
- **Add relation** (`###rel_add`, under the Relations list) opens the same
  window in create mode (`AppState::asmNewRelation`, the draft in
  `relationDraft`): a Kind combo (`###rel_kind`: gear, internal, linear,
  epicyclic, geneva, contact), the members, the teeth (a Geneva's
  `###rel_slots` and `###rel_start`; a cam contact's `###rel_follower_moves`,
  Slides or Turns, and `###rel_toward`, From gravity, Positive or
  Negative), and for an epicyclic a Planets list
  with `###rel_planet_add`. What the draft lacks is the red line
  (`asmEditRefusal`) and Accept (`###rel_new_accept`) stays disabled
  ([relations.md](../assemblies/relations.md#in-the-app)).

Floating from the assembly toolbar, one at a time through the tool-window
slot (an exploded view and a motion study both place occurrences, so holding
both open means nothing):

- **Exploded views** (`###tb_asm_explode`, `assembly.explode`, X) —
  `DrawExplodeToolWindow`: the list, Add, the name, the factor slider,
  auto-explode, Delete. Close does not clear the view:
  `activeExplodedView` stays applied, because the explode is a display state
  and a person who closes the window is done editing, not done looking. The
  gizmo's arrows write the selected occurrence's step; with a moving part of
  a flexible placement selected they write that part's own step, named by
  path.
- **Motion** (`###tb_asm_motion`, `assembly.motion`, M) —
  `DrawMotionToolWindow`: the study list, the driver, the range, the Pose
  scrub (`###motion_t`), and:
  - The driver combo (`###motion_driver`) and Add study take their mates
    from `ui::DofMateChoices`, so a study can be driven by a mate inside a
    flexible placement (stored as `driver_path`).
  - **Preview motion** and **Validate motion** are *runs*, not edits: each
    passes a `RunOverride {checkClash, clashMode}` to `StartMotionRun`
    applied to the run's copy of the study, so the stored study (and
    therefore what `run_motion_study` does) is untouched and no undo step is
    pushed.
  - The verdict is `motion::Verdict(result, study, stale)`
    (`core/MotionStudy.h`), which asks whether the samples **solved** before
    it asks about collisions ("Does not solve at sample 19 …" / "Moves
    through all 73 samples; collisions not checked" / the clashing pair).
  - **Play/Pause** animates the viewport through the run's stored poses,
    display-only (`DisplayMatrix` interpolates; `TickMotionPlayback` runs
    beside the CAM playback tick in both main loops). Play is refused, with
    the reason, without a fresh solved result. Stopping restores the
    document transforms; anything that invalidates the result stops
    playback.
  - **Export GLB…** (`###motion_export`) saves the run as an animated GLB
    (`ExportMotionGlb`), and a refusal is shown in the panel.
  - **Export animation…** (`###motion_video`) films the study to mp4, webm,
    gif or png frames, the save dialog's file type picking which
    ([motion video](../assemblies/README.md#motion-video)): the study as Play
    plays it, each frame the viewport's own picture. It needs no run.
    While the `motion-video` job runs it is the job's bar and Cancel
    (`###motion_video_cancel`); the last outcome is shown under it. GUI tests
    answer its dialog with `AppState::saveDialogAnswer` instead of opening
    the desktop portal.
  - **Assembly sequence**, under the studies when the assembly has one
    (`DrawSequenceSection`): **Play/Pause** (`###sequence_play`) checks the
    sequence (a bar with Cancel, `###sequence_cancel`, while it runs) and
    plays it going together, display-only; the **Time** scrub
    (`###sequence_t`) holds it at a moment; **Stop** (`###sequence_stop`)
    puts the display back. A sequence that does not check is shown as the
    reason it was not played
    ([playing and filming a sequence](../assemblies/README.md#playing-and-filming-a-sequence)).
- **Simulation** (`###tb_asm_simulation`) — `DrawSimulationToolWindow` (in
  `StudyPanel.cpp`): the studies over `ui::StudyHost`, plus a participants
  table (each top-level occurrence's role: Ignored, Rigid, Meshed or Bonded;
  "Clamp here" on a rigid row). Close leaves simulation mode.
- **Bill of materials** (`###tb_asm_bom`, `assembly.bom`, B) —
  `DrawBomWindow`, floating below the tool window: name, quantity, material,
  mass and total as sortable columns with the total mass in the header, plus
  Check fasteners and Check fits.

An assembly's configurations are in the Configurations dock tab
(`DrawConfigurationsPanel` calls `DrawAssemblyConfigs` on an assembly tab),
browsed and switched as a part's are.

## CAM job tab: the CAM panel

The job as a Model tree, like the part and assembly panels
(`src/ui/CamTree.cpp`; the operation row, the settings forms the tool window
draws and the backplot stay in `CamPanel.cpp`). Three `SectionHeader`s with
counts, each row a `ListRow`:

- **Parts** — a cube glyph, the part's name, its configuration as the tail
  (else its material's name, "Oak", not the slug "wood_oak"). A part whose source is gone, or whose configuration
  no longer exists or does not build, is red with the reason in the tooltip;
  one that does not fit the active setup's stock (`CheckPlacements`) says so
  as its tail ("hangs over the edge"). Dimmed when the active setup leaves it
  on the sheet. Click selects it (`SelectionItem::Kind::JobPart`) and its
  bodies, which gives it the placement gizmo;
  double-click edits its placement in the tool window
  (`DrawCamPartEditor`: its configuration, Cut, X, Y and turn). Right-click:
  Edit placement, **Configuration** (a submenu of the part's configurations,
  `###cam_part_cfg_<id>`, 0 = its active one), Place or Cut, Remove (the
  selected parts, `RemoveJobParts`). The
  header's `+` (`###cam_parts_add`, shown on hover) offers each project part;
  one with configurations is a submenu of them (`###cam_part_add_cfg_<id>`),
  so one part can be cut in several (the chess undersides).
- **Setups** — each a row with a disclosure (`###cam_setup_open_<id>`) over
  its operations and fixtures, so several setups are open at once. A plain
  click selects the row (`Kind::CamSetup`) and makes it the **active setup**,
  opening it; what the toolpaths, verbs and editors act on. The lit rows are
  the selected setups, or the active one while none is. The active setup's tail
  is the run status as a glyph (`###cam_run_status`: not generated,
  generated, verified ok, warnings, failed, working), which opens the
  Generate window; another setup's tail counts its operations.
  Double-click opens its settings in the tool window (`DrawCamSetupEditor`:
  name, machine and its reconciliation, stock and safety, post, the rotary,
  its fixtures). Right-click: Settings, Rename (in place), Flip, Duplicate,
  Add operation (`cam::AddOperationType`), Add fixture (a submenu), Delete
  (the selected setups, `RemoveCamSetups`). The header's `+` adds a setup.
- **Tools** — the active setup's, one `ListRow` each (number and type glyph,
  description, diameter as the tail; `Kind::CamTool`, by number); double-click
  edits in the tool window. Delete is disabled while an operation uses the
  one tool right-clicked, with those operations named, and `RemoveCamTool`
  refuses with the same sentence; of several selected, `RemoveCamTools`
  removes the free ones and says why the rest stay. An edit that
  would drive the tool's shank into a wall in one of those operations is
  taken back, the reason in `camRefusal` ([CAM setup](../cam/setup.md#reach)).
  The header's `+` offers the machine's library, then a blank tool.

Under a setup:

- **Operations** are rows (`CamOperationRow`) with the feature list's
  gestures: click selects, drag reorders within the setup, double-click
  opens the operation's settings (`DrawCamOpSettings`) in the tool window,
  right-click offers Edit, Enable/Disable, "Show what it removes" and Delete.
  A disabled operation is dimmed. The summary is type-aware (a rotary
  operation states its pattern, lead and stepover). Editing an operation of
  another setup makes that setup active first.
- **Fixtures** — a glyph, the name, where it sits as the tail
  (`Kind::Fixture`); right-click Edit, Check, Move to, Remove (the selected
  fixtures, `RemoveCamFixtures`).

Every row kind is a list's row (docs/ui/selection.md#lists-one-gesture-one-delete):
Ctrl toggles, Shift spans, and the **Delete key** removes whatever of the five
kinds is selected, as one undo step. A plain click on one kind lets go of the
others (`ui::CamRowClicked`), so what Delete takes is what is lit.

A job with no setup yet shows the setup step instead
([gui.md](../cam/gui.md#making-a-job)). **Generate** is a window, not a panel
block (`src/ui/CamGenerate.cpp`): the toolbar's gears (`job.generate`, G)
always regenerate and open it; the status glyph opens it as it stands. The
CAM verbs themselves (docs/cam) run in the tool window. Starting a verb
closes an open editor; an editor takes the window from a verb.

## Simulation: the Study panel and window

On a part tab the studies are listed in the dock as `ListRow`s (a glyph for
static, modal or topology, the name, and the state — solved, stale, failed —
as the tail). A double-click, or the toolbar's Run (`###tb_sim_run`), opens
the study in the floating Simulation window, where it is edited, run and read
with the solve in the shared progress bar. Loads and kept faces are rows with
Remove in their menu.

What the setup draws in the viewport (`src/ui/StudyPanel.cpp`):

- Every boundary-condition face is tinted through the mesh renderer's
  face-highlight channel in its family's colour (fixtures, loads, keep
  faces; `MeshRenderer::HighlightTint`), by `AppendSimulationFaceTints`,
  which is part of the selection projection because that projection clears
  first.
- A pressure is drawn as a field: `SampleFace` lays sample points over the
  face and each gets a short arrow along its own triangle's normal; a fixed
  support repeats its anchor hatch on the same grid. A force keeps one arrow,
  which is its drag handle ([viewport-handles.md](viewport-handles.md)).
- Gravity is an arrow from the centre of mass.
- Each boundary condition carries a label with its value and unit
  (`SimulationLoadLabel`: "1.2 kN", "20 kPa"), switchable with "Labels in
  viewport".

Defaults come from the part: a force's direction is the negated outward
normal of the face it is placed on (`fem::DefaultForceDirection`, shared with
`add_load` when no direction is given; "Custom direction" restores an
explicit vector), and its magnitude starts at the part's own weight. A body
still on the default material (which has no stiffness data, deliberately) is
flagged in the panel before Run, with the material combo in place
(`###material_<body creator id>`). The load list shows each load's magnitude
and direction. The `--offscreen` instance builds no overlays, so a study's
picture needs a windowed instance.

## Drawing tab: the Drawing panel

`src/ui/DrawingPanel.cpp`. Views and dimensions are `ListRow`s (a view's tail
is its scale, a dimension's its value). A view's menu sets its **Scale**
and **Rotate** (0/90/180/270), adds a bend table (when the view has bends),
starts a **Detail view** of it, **Lays out balloons** round it (an assembly
view) and deletes; a dimension's menu starts a **Feature control frame**
under it and deletes. Those are the drawing tab's select-first: rows have the
menu gestures but no selection, and a click highlights nothing on the sheet.
What the last balloon layout did is a line under Views. **Add view** is one path: the toolbar opens it in the
tool window (`DrawAddViewToolWindow`, `drawingAddViewOpen`) with the source,
configuration, all seven orientations and the scale. Dimensions, Update
views, the part note (`###tb_dwg_part_note`, L), the detail view
(`###tb_dwg_detail`), the frame, the datum and the balloon layout
(`###tb_dwg_fcf`, `###tb_dwg_datum`, `###tb_dwg_balloons`) are on the
drawing toolbar ([../drawings/guide.md](../drawings/guide.md#in-the-gui));
the panel keeps the count of stale views as a line pointing at the toolbar.
While a `drawing` job projects this sheet's views, the view list carries
its progress -- the shared `DrawJobProgress` bar, which view and which pair,
and Cancel (`###dwg_cancel_projection`) -- rather than a modal, and a view
row's tooltip says it is projecting
([drawings](../drawings/README.md#projection-off-the-frame)).

## Other dock panels

Variables (with the document's Units section), Configurations, History,
Conflicts, Requirements ([the panel](../requirements/README.md#the-panel)),
the CAM panel on a job tab, and the Terminal (right-hand node,
[terminal.md](terminal.md)). Their titles and ids are in
`src/ui/PanelTitles.h`. The Design sweep window
(`src/ui/DesignSweepPanel.cpp`) opens from a variable's "..." in the
Variables panel and from the Requirements panel's "Sweep the ranges"
([design sweep](../requirements/README.md#design-sweep)).

## Not built

The Configurations panel's checkbox columns and the History panel's row
buttons do not yet follow the row pattern; a shared radio row for 2–4 choices
(the feature editor, CAM, Settings) is not built; Delete does not reach
assembly relations, nor the CAM panel's setups, parts, fixtures and tools
(only its operations are in the shared list selection; the others delete
from their menus); a placed simulation load cannot be edited in place, only
removed and replaced.
