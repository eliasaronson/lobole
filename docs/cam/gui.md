# CAM in the GUI

The design rule: **the app knows the answer, so it does not ask.** A CAM
beginner has a part and an intent — clear that pocket, cut this out, face the
top — not depths. So the way in is a verb and a click on the model, every
number is derived ([operations.md](operations.md#derivation--what-a-click-means))
and stays visible and editable, and the verdict is in words. Two clicks make a
pocket and nothing is typed.

Code: `src/ui/Toolbar.cpp` (`DrawJobToolbar`), `Commands.cpp` and
`core/io/Keymap.cpp` (`job.*`), `CamTool.{h,cpp}` (the verb session, its
windows and what it draws), `CamPanel.{h,cpp}`, `CamGenerate.{h,cpp}`,
`CamPlayback.{h,cpp}`, `GizmoInput.cpp`, `SettingsWindow.cpp`.

## Making a job

A job is made the way an assembly is: **New Job** on the tab bar's `+`,
beside Part, Assembly and Drawing. On a job with no setup, the CAM panel is
the setup step:

1. **Machine** — chosen for you when there is one; with none, a refusal
   pointing at Preferences › Machines.
2. **Stock** — *Around the parts* (the parts' box plus margins) or *A sheet
   you own*, whose size defaults to the machine's bed and whose thickness
   follows the parts. No literal anywhere.
3. **Parts** — the project's parts, each added with a button.
4. **Create setup.**

`machine_part` is the RPC composite of making a job and adding a part.

## The job toolbar

A job tab has its own toolbar; every button is a command (`job.*`, keymap
context `job`), so keys, the command palette, tooltips and the bar agree.
Button ids are `###tb_job_*`.

| group | verb | command | key | pick |
|---|---|---|---|---|
| Set up | Pick the top face (Z zero; also lays the part flat on it) | `job.top_face` | Z | a level-able face |
| | Lay a face up (how the part is clamped) | `job.lay_face_up` | L | a planar face |
| | Turn a face up (rotary table) * | `job.index_face` | A | a face |
| | Look down the spindle | `job.look_down_spindle` | T | — |
| Machine | Clear a pocket | `job.pocket` | P | its floor |
| | Cut it out | `job.profile` | C | the wall to follow |
| | Drill a hole | `job.drill` | D | inside the hole |
| | Break an edge | `job.edge` | B | the wall whose top edge to break |
| | Engrave | `job.engrave` | E | a label or a curve drawn on the part ([below](#engrave-from-the-viewport)) |
| | Cut out the whole part | `job.whole` | Shift+C | none |
| | Face the top | `job.face` | Shift+T | none |
| | Rough a surface | `job.surface_rough` | Shift+S | none |
| | Finish a surface | `job.surface` | R | a curved or sloped face |
| | Finish walls | `job.walls` | W | a face (constant-depth laps) |
| | Rough on the rotary * | `job.rotary_rough` | Shift+O | none |
| | Finish on the rotary * | `job.rotary_finish` | Shift+R | none |
| | Clean up what's left | `job.rest` | Shift+E | none |
| Output | Generate toolpaths and verify | `job.generate` | G | — |
| | Simulate the cut | `job.simulate` | S | — |
| | Fit, Measure | `view.fit`, `view.measure` | | |

\* Only on a setup whose machine has a rotary axis: a disabled button for an
axis you do not own is a question with no answer, and offering the index
unconditionally is how a 3-axis job could be handed `G0 A90`.

**Look down the spindle** gives the plan view of this setup framed on the
stock — the view down the spindle in the frame this setup cuts in, which is
the world's Top only when the part is clamped as modelled and the table is
square.

## Verb windows

Pressing a verb opens **that verb's window** — its sentence, its prompt, the
**tool** it derives for, and the operation it made — and lights the button
while its pick is armed.

- **A verb stays armed.** One verb is rarely one face (six holes to clear), so
  the pick survives a success; Escape or pressing the verb again puts it
  away. Other verbs' buttons are not greyed while one is armed.
- **The no-pick verbs** make the cut at once and open their window so the tool
  can still be changed.
- **Changing the tool re-derives the cut just made** — every derived number is
  a fraction of the cutter — rather than asking for a remove and redo. The
  tool defaults to the last one used (`ui::VerbTool`): the one chosen this
  session, else the setup's newest operation's, else the first. It is read
  from the job, so it holds across sessions.
- **The rotary verbs** add **Limit to a face** (arms a surface pick and
  bounds the pass along the axis to that face) and **Whole length**.
- **The window lists the setup's operations** with the panel's own row
  (`CamOperationRow`), so an operation is removed (right-click Delete) where
  it was made, without switching to the docked panel.
- **Refusals** are wrapped text that says what to click instead, and the
  prompt pill names the pick that is actually armed; a pick that hits nothing
  says "Nothing there".
- **Pick the top face** lays the face up (for a face already level nothing
  moves), takes Z zero from it and re-derives safe Z; re-clamping re-derives
  every targeted operation's depths and says how many moved. It says nothing
  about how much material sits above the part: the stock's Z margin (default
  1 mm, the rough board a facing pass exists for) is kept, so Face the top
  works right after the pick. A pick on a job with no setup makes the setup.
- **Session state belongs to the job.** A new document, an opened project or
  a closed job tab ends every CAM session with it — an armed pick, an open
  verb window, a playback — so none is left pointing at a job that is gone.
- The window follows the viewport and re-anchors when it reopens; fitting the
  camera keeps the model in the band the window leaves.
- **It keeps a bounded height and scrolls past it**: half the viewport's
  height (never less than eight rows). It grew with what it lists, and
  Engrave's -- an operation per label, the verdict's every line -- grew down
  over the curves it was asking to be clicked
  (`cam/the_engrave_window_keeps_its_height`).

## Engrave from the viewport

A job tab draws each job part's **sketches that hold a text label or an open
curve** (`cam::EngravableEntities`: a curve no closed profile of its sketch
uses) — every cuttable entity in such a sketch, laid out through the part's
placement in the active setup (`BodyToSetup`, the transform its bodies take)
as thin lines in the sketch colour. A sketch of closed profiles only is some
feature's outline, not a marking, and is not drawn. The lines are pickable,
not editable: the sketch is edited in its part.

**Engrave** (`job.engrave`) arms a pick of those lines
(`SelectionItem::Kind::SketchCurve`: the nearest drawn curve within a few
pixels, not one the part hides; the hovered one lights). A click derives the
operation (`DeriveOperation` for `engrave`: the groove's width from the
label's cap height, its depth from the V-bit,
[operations.md](operations.md#engrave)); a click on another label adds it to
the same operation, and while the window shows it its labels are drawn in the
ghost's colour. The verb cuts with the setup's V-bit (the verb's tool when it
is one, else the first) and its tool row offers only V-bits. **A setup with
no V-bit is refused where the verb was pressed**, naming the tools it does
have, and nothing is armed.

## Indices from picks

On a setup held in the rotary (`cam::HeldInRotary`), a floor verb (Clear a
pocket) or Engrave whose pick does not face the spindle at the setup's index
is not refused as a wall: the index that turns it up is solved
(`cam::SolveIndexA` on its normal in the setup's frame at A = 0, as **Turn a
face up** reads it; an engraving's by its sketch plane) and the operation is
derived in that frame, cut at that index. A further pick of the same verb at
another index adds the index and the target to the same operation
(`cam::AddIndexedTarget`: the setup's own index and the new one, sorted,
`cam::NormalizeIndexDeg` into [0, 360)), so four crenel floors picked one
after another are one pocket at `[0, 90, 180, 270]`
([rotary.md](rotary.md#one-operation-at-several-indices)). A floor joins the
operation only when it is the same cut — its depth, derived where it faces
the spindle, is the operation's own, since every index is cut to one depth;
another depth makes an operation of its own. A tool change in the verb's
window re-derives in the frame of the first index and keeps every target and
index.

## Milling a thread

A thread has no verb on the toolbar: it is picked by what it is, not by
where it is. A setup's menu in the CAM panel has **Add operation › Mill a
thread**, listing every thread on the job's parts -- each Thread, and each
bore of a Hole with a tapped fit -- by its designation, class and side
(`###cam_thread_<n>`). Choosing one derives a `thread_mill` through the same
`cam::DeriveOperation` the RPC uses, with the setup's first thread mill; a
refusal (no thread mill in the setup, a cutter too large for the bore) is
shown in the panel. In the operation's settings the depths are shown and not
editable -- they are the thread's -- and **Passes** (0 = derived) sits beside
the climb and lead-arc toggles ([operations.md](operations.md#thread-mill)).
The tool's settings take the thread mill's **flat**, **neck** and **reach**
when its shape is *thread mill*; switching a tool to it derives the head
from its diameter. Playback draws the helix like any other path.

## What the viewport draws

Show only the consequence of the decision being made, and drop it when the
decision is made.

- **Always, on a job tab:** the parts' labels and open sketch curves, thin in
  the sketch colour ([above](#engrave-from-the-viewport)); the parts laid out in setup space; the stock as a
  translucent box; the machine's work area as a dimmer box (red when the job
  does not fit it; never resized); the fixtures, ghosted grey (outside the
  document's body list, so picking and selection indices are untouched); the
  backplot of the generated toolpaths — cuts amber, plunges red, rapids muted
  grey — handed to the renderer's cached channel
  (`SketchRenderer::SetCachedPrimitives`, a shared immutable list uploaded
  only when it changes, never copied per frame).
- **Framing.** Fit (`job.fit`, F), the first visit to a job tab, and opening
  a project whose active tab is a job frame the parts laid out, by their
  EXACT boxes, with the stock round them and the fixtures
  (`ui::VisibleSceneBounds`, the stock from `ui::CamStockBox`). The
  conservative box bounds a turned blank's B-splines by their control points
  and framed the chess jig's piece far bigger than it is; a project opened on
  a job tab was not framed at all
  (`cam/a_job_tab_frames_its_parts_and_stock`).
- **While a verb is armed:** a hover preview of the region the click would
  clear.
- **At the pick:** the tool to scale where it first enters the material (will
  my cutter fit, and what radius will it leave?), the material that will be
  removed as a ghost from the operation's top to its floor, and the depth as a
  label on the part.
- **After Generate:** the leftover painted on the part as points (not for a
  rotary job).
- An operation row's **Show what it removes** ghosts that operation's removed
  volume on demand.

## The CAM panel

The panel is the job's STRUCTURE and exists only on a job tab; the run is a
tool. It is a Model tree like the part and assembly panels
([panels.md](../ui/panels.md#cam-job-tab-the-cam-panel)): Parts, Setups and
Tools headers with counts, rows with a glyph, a name and a dim tail, the
create verbs on the headers' `+`, the rest in each row's menu, settings in
the tool window.

- **Parts:** each job part with its configuration (or its material's name,
  "Oak", not the slug the file stores) as the tail;
  a part whose source is gone, or whose configuration is gone or does not
  build, is red with the reason. Right-click: Edit placement,
  **Configuration** (a submenu of the part's configurations, through
  `ui::SetJobPartConfiguration`: one undo step, and every setup's origin,
  safe Z and targeted depths re-derived as a re-clamp re-derives them; the
  RPC's `set_job_part` is the same call), Place (a part without a placement
  in the active setup) or Cut (off leaves it on the sheet), Remove (the
  selected parts; their placements go, and no operation cuts them any more).
  Double-click edits the placement in the tool window: the configuration,
  Cut, and its X, Y and turn, named inside the fields, with the fit said
  under them. The header's `+` opens a menu of the project's parts: one
  without configurations while the job does not hold it, one with them as a
  submenu of its active one and each configuration, so a part can be added
  once per configuration (the chess undersides: one piece part, a blank per
  rank).
- **Setups:** a row per setup with a disclosure over its operations and
  fixtures; a click on one selects it, makes it the active setup (it was a
  combo) and opens it. The lit rows are the selected setups, or the active
  one while none is selected, so what Delete would take is what is lit. The active setup's tail is
  the run's state as a glyph — not generated, generated but not verified,
  verified ok, warnings, failed, working — and a click on it opens the
  Generate window as it stands. Right-click: Settings, Rename, **Flip** (the
  other side, derived), Duplicate (operations, fixtures and placements
  included, each with a new id), Add operation, Add fixture, Delete (the
  selected setups). The header's `+` adds a setup.
- **A setup's settings** (double-click, or Settings) open in the tool
  window: its name; the machine combo, and under it the reconciliation line
  when the embedded machine differs from yours or is not one of yours (*Use
  mine*, *Save as mine* / *Save it*; [setup.md](setup.md#machine-profiles));
  **stock and safety**: safe Z; the stock mode (Around the part / A fixed
  sheet) with margins or size and place; where zero is (Stock corner, Stock
  centre, Part corner) with the resulting origin printed beside it; the
  job's copy of the machine's post, arcs, 4th axis and A index; on a
  4th-axis machine, *Part held*, *In the chuck*, *mm in the jaws* and
  *Tailstock engaged* ([rotary.md](rotary.md#chuck-and-tailstock)); and the
  fixtures it has (none: verify checks no clamps). Each edit is one undo
  step, as it was in the panel.
- **Fixtures,** under their setup: one row per fixture — a glyph, its name,
  where it sits as a dim tail ("−X side", "over the −X edge", or "its seat
  is gone: …"), greyed when not checked; right-click Edit, Check, Move to
  (−X, +X, −Y, +Y, Centre, Top) and Remove (the selected fixtures). The setup's **Add fixture**
  offers the keep-out box first, then "Hold it with a part:" and the project
  parts the job does not cut; each item's tooltip says what the choice does,
  so cutting a part and clamping with it are not confused.
- **Tools:** the active setup's, rows in the list style (right-click Edit,
  Delete -- disabled for one an operation cuts with; of several selected, the
  free ones go and the rest are refused in words),
  the header's `+` offering the machine's library first, then a blank tool.
  Feeds and speeds are asked for, and the panel says why: they are the one
  number that is genuinely the user's. The editor has the shank's width
  beside stickout and collar; an edit that would leave an operation with its
  shank driven into a wall is taken back, with the reason shown
  ([setup.md](setup.md#reach)).
- **Operations,** under their setup: rows in the feature list's style,
  reorderable by the grip within their setup (the order is the cutting
  sequence); double-click opens the op's settings in the CAM tool window;
  right-click Edit, Enable/Disable, Show what it removes, Delete. A depth or
  tool its cutter cannot reach is taken back with the reason shown, and a
  reach that is only just, or rubs, is said under the settings
  ([setup.md](setup.md#reach)). The settings show only the fields the type
  has (the capability table): tool as `T1 T2 …` buttons, depths,
  stepdown/stepover, leave, tabs, corner relief, finish tool, lead arcs,
  **Comp** (`###cam_comp`: cutter compensation in wear mode on a profile,
  pocket or drill, [post.md](post.md#cutter-compensation)), Over openings,
  the rotary pattern as a radio pair, an engraving's width (for a V-bit) and
  depth, and so on. On a machine with a rotary axis the indices the
  operation is cut at are **chips**, `A0 A90 A180 A270`
  (`###index_chip_<angle>`), each with an x that stops cutting there; empty
  says the setup's own index
  ([rotary.md](rotary.md#one-operation-at-several-indices)). They come from
  picks ([above](#indices-from-picks)); a small **edit** (`###index_edit`)
  opens the typed list (`0 90 180 270`, `###index_a`) for an angle no face can
  be picked at. The row's summary ends "at A0/90/180/270"; an engraving's says
  its labels, width and depth. A setup's **Add operation** adds what the
  setup calls for (`cam::AddOperationType`: another rotary pass on a setup
  that already cuts on the 4th axis, else the whole-part cut-out) through
  `DeriveOperation`, and shows a refusal instead of adding a dead operation.

**Every row kind is selected and deleted as a list's rows are**
([selection.md](../ui/selection.md#lists-one-gesture-one-delete)): a click
selects, Ctrl toggles, Shift spans, and the Delete key removes what is
selected -- operations, tools, fixtures, parts and setups, in that order, as
ONE undo step (`ui::DeleteSelection` over `RemoveCamOperations`,
`RemoveCamTools`, `RemoveCamFixtures`, `RemoveJobParts`, `RemoveCamSetups`).
A row menu's Delete or Remove takes its own kind only, since it names it. A
plain click lets go of the panel's other row kinds -- a setup picked a moment
ago must not go with the tool Delete was pressed for -- and keeps picked
faces, which the verb windows are aimed at (`ui::CamRowClicked`). A tool row
is the active setup's, so making another setup active lets go of selected
tools.

Combos with an empty label cannot be addressed by the GUI test engine, so
choices a test must make (add menus, the rotary pattern, tool buttons) are
popups, buttons or radio pairs. No two visible widgets may share an ImGui id:
ImGui reports a collision in a popup no test sees, so
`cam/panel_widgets_have_distinct_ids` draws the whole panel and fails on one.

## Undo

Every job edit is one undo step (router-round R7): a verb, a pick, a panel
field, a drag, a menu verb, a Delete. The panel, every editor in the tool
window (setup, part, operation, tool, fixture) and each verb draw under a
`ui::JobUndoGuard`, which copies the job and, when
it closes, pushes the copy if the job was edited in between; only the
outermost guard records, so an action with a guard of its own called under
another is still one step. A drag (`coalesce`) is one step from where it
started, unless it came back there. So nothing under a guard takes its own
snapshot (`SnapshotJob`): that would be a second step for one edit. The
operation, tool and fixture editors once drew under no guard, and a changed
depth, cutter or clamp could not be taken back (`cam/an_editor_field_undoes`).

**"Edited" is a counter, not the job's content.** Every job edit calls
`ui::MarkJobEdited` (it bumps `AppState::jobEdits` and the job's save stamp,
which is what makes the project unsaved and the job autosave —
[versioning](../versioning/README.md#unsaved-changes)), and the guard
compares the counter. It compared the job serialized
to JSON before and after every frame -- 8% of an idle frame, for an answer
that was almost always "no". Nothing a frame reads moves the counter
(`tests/test_actions.cpp`, "a job edit bumps the edit counter and a draw does
not"). The RPC guard compares the content (`compareContent`): it runs once
per call, not per frame, and wraps a hundred handlers, none of which then has
to say it changed something: a guard that records a step marks the job
edited itself.

**The GUI suite audits the rule.** Under `--gui-tests` every guard also
compares the content (`ui::SetJobEditAudit`), and a test in which the job
changed without the counter moving fails, naming the first difference in the
job's JSON: that edit would not undo.

`RemoveCamOperations` and `RemoveCamTool` hold a guard rather than taking a
snapshot. They used to snapshot, and under the panel's guard a delete from
the panel's menu was two steps, the first of which brought back nothing
(`cam/a_delete_from_the_panel_is_one_undo`).

## Job tab frame cost

A job tab draws its parts laid out in the active setup, and the stock, the
machine's box and the camera fit read their EXACT bounding boxes -- on a
turned blank's tori, a search per face. The box is memoized on the shape
(docs/modeling/README.md#bounding-boxes-are-memoized-on-the-shape), and a copy moved by
a quarter turn carries it, but a part indexed at any other angle is a new
shape each time the tab frames it: a job at A = 30 paid its boxes again on
every visit, 210 ms on the frame-time test's shaft and most of a second on
the chess blanks.

- **The tight box is also remembered by shape identity and location**
  (`CADShape::GetBoundingBox`, `src/cad/CADKernel.cpp`): the same TShape at
  the same placement is the same point set whichever `CADShape` holds it, so
  a framing done again -- the tab revisited, the stock derived from a fresh
  framing, the machine's box, a region pick -- reads the box the first one
  paid for. Only located shapes are stored; each entry holds its TShape, so an
  address is never reused under it, and the ring is bounded (1024), as the
  volume cache's is. Opening a project forgets them.
- **The first visit's cost is paid off the frame.** Opening a project starts
  a `job-warm` background job (`ui::StartJobBoundsWarm`): every job tab but
  the active one is framed in each of its setups on the main thread (a
  `Moved`, no geometry), each source body is deep-copied once
  (`CADShape::DetachedUnlocated`, so the worker never reads a TShape the
  frame may be meshing), and the worker bounds the copies at the framings'
  locations (`CADShape::ExactBoxUnmemoized`). Its `finish` hands the boxes to
  the memo under the real framings' identity (`CADShape::RememberTightBox`).
  A part in a configuration not built yet is left to the visit, which builds
  it: building it at open would be a stall of its own.

`frametime/job_tab_idle_and_switch` holds the revisit to a bound
([frame-pacing.md](../rendering/frame-pacing.md#tests)).

## Generate window

The toolbar's Generate (and the active setup's status glyph) opens a tool window with the run and what it
found: generate and verify, the verdict sentences (Good/Warn/Bad, the same
text `cam_verify` returns as `says`) with the cutter findings and machine
lines, the output options — cutting order (`Cut`: authored/tool/part) and
**One file per tool** — **Show path**, **Save G-code**
([post.md](post.md#files)) and **Setup sheet**, the page for the machine
written beside the project ([setup.md](setup.md#the-setup-sheet)). A generate on a job tab defers the slow passes to
the background ([README.md](README.md#background-work)); the toolpaths fill in
as they land and a progress row shows the batch. The "N mm of cutting" total is
cached on the toolpaths' key.

### Whose results are on screen

Everything the window shows — the verdict, the "N mm of cutting", the saved
file's path — and the viewport's leftover dots and fixture-hit marker are
about ONE setup of ONE job tab: `AppState::camResultOwner`, the project, the
job's slot id and the setup id (identities, not a version). They were global
and "cleared on any edit", and a switch to another job or setup is not an
edit, so job B's window went on showing job A's verdict, cutting length and
`.nc` path, and A's backplot was drawn over B.

`ui::SyncCamResultOwner` (CamPanel.cpp) keeps them honest. A tab switch calls
it, as does `RefreshCamToolpaths` and every reader (this window, the CAM
panel, the tool window, the viewport overlay, a playback starting). When the
setup on screen is not the owner, it:

- keeps the last setup's run with that setup — in `camSetupResults` while its
  job is the active tab, in its `PartSlot::camResults` once it is not, the
  way a drawing's renders are kept per slot. Only a finished verdict is kept
  ("Verifying..." would say so forever);
- drops the notes and refusals, and a verification still on a worker (its
  generation no longer matches), and ends a playback of the other setup's file;
- puts the new setup's own run back: the saved path always (the file was
  written), the verdict, leftover and fixture hit only while `CamVerdictKey`
  — the job's save stamp, which every recorded edit moves, and `JobPassesKey`,
  the bodies it cuts and where — is what it was when the verdict was asked.
  An edit to the job, or to a part it cuts, while that setup was away leaves
  "Not verified since the last change";
- regenerates the toolpaths when they were made for another owner.

Which setup is active is kept per job tab too (`PartSlot::camActiveSetup`):
setup ids count from the same start in every job, so a global one opened the
next job on its setup of the same id.

## Simulate

`job.simulate` opens the transport window along the bottom of the viewport
([simulation.md](simulation.md#transport)). With nothing generated it refuses
in a popup at the click. A setup with earlier setups in the job first reads
what they left, on a worker ("Reading what the earlier setups left"), and the
transport opens when that lands, saying "Starts from what 'Turn' left"
([simulation.md](simulation.md#leftover-another-setup-removes)).

## Gizmos

- **Placement.** Selecting a job part gives it the job tab's gizmo at its top
  centre: X and Y arrows move its placement in the stock, the Z arc spins it.
  Nothing out of plane — which face is up is a face pick. A part without a
  placement gets one on grab. Unless Ctrl is held, its edges snap to the
  sheet's edges and centre and to other parts' edges, and the spin snaps to
  multiples of 45° within 5° (`ui::SnapAngleDeg`). The drag redraws each
  frame; the drop regenerates and re-verifies; one undo step per drag.
- **Fixture.** While a fixture's editor is open it gets X, Y and Z arrows and
  a spin about Z (it never tilts). Unless Ctrl is held, its own flat faces
  square to the dragged axis (gathered once at grab: a step clamp's pad, bar
  and stud undersides are separate candidates) snap to the stock's planes on
  that axis — both sides in X and Y, top and bed in Z — within a screen-sized
  threshold, and the editor names the snap ("Pad's underside on the stock
  top"). Released with a flat, named, downward face on the stock's top and
  over it, the fixture is **seated** (`cam::SeatFromPose`) and follows the
  stock from then on; dragged off the top in Z it unseats. A seated fixture's
  editor shows the edge, Along, Over the stock and Spin, measured from the
  stock, and Unseat; an unseated one shows its bed position and the
  placement buttons.

## Preferences › Machines

The machine store's editor, a library page
([settings.md](../ui/settings.md#library-pages)): the list of machines on the
left under New / Duplicate / Delete, and the selected one's form beside it,
whose every edit writes the file (a rename moves it): name, work area X, Y, Z
(a partly typed area is kept), post, arcs, 4th axis, how the rotary is
installed and its chuck and tailstock
([rotary.md](rotary.md#how-the-rotary-is-installed)), rapid feed, the A
axis's top speed, and the tool library. Numeric fields go through `ui::ExprInput`.
The CAM panel's `Preferences › Machines` buttons open the window on this page
(`ui::OpenPreferences`).

## Tests

`src/app/guitests/GuiTestsCam.cpp`, e.g. `cam/verbs_two_clicks_to_a_pocket`
(verb, face, a derived pocket with a toolpath; the ghost's Z span; the session
closing), `cam/verbs_two_clicks_to_an_edge_break`, `cam/upright_panel_lays_flat`
(a panel standing on edge: Z zero refuses its big face, laying it up makes it
pickable, no A word is posted), `cam/job_tab_has_its_own_toolbar` (the row
fits, no two verbs share a glyph), `cam/a_job_is_made_from_the_tab_bar`,
`cam/a_placement_is_dragged_in_the_viewport`,
`cam/a_clamp_is_dragged_onto_the_stock`, `cam/add_menus_offer_the_right_parts`,
`cam/the_surface_verbs_take_a_curved_face`,
`cam/a_label_is_engraved_by_clicking_it`, `cam/engrave_without_a_v_bit_refuses`,
`cam/a_thread_is_milled_from_the_setup_menu` (the menu's thread, the op's
tool, its declared helix, and its passes edited),
`cam/four_floors_picked_are_one_op` (the chips, and the typed list behind
edit),
`cam/the_rotary_verbs_rough_then_finish`, `cam/a_rotary_pass_limits_to_a_face`,
`cam/backplot_is_not_copied_per_frame`, `cam/a_job_edit_undoes`,
`cam/two_setups_are_open_at_once`, `cam/one_part_in_two_configurations`
(one part added twice in two configurations, one pocketed and generated),
`cam/delete_reaches_every_row_kind` (a tool, two tools at once, a fixture, a
part and a setup, each by the Delete key, each one undo step that brings it
back), `cam/a_delete_from_the_panel_is_one_undo`,
`cam/the_engrave_window_keeps_its_height`,
`cam/a_job_tab_frames_its_parts_and_stock` (the first visit, and a project
opened on the job), `cam/another_job_shows_its_own_run` and
`cam/another_setup_shows_its_own_run`
([Whose results are on screen](#whose-results-are-on-screen)). A test that
leaves a CAM window open, or a docked tab switched, breaks later tests' clicks,
so cleanup that matters to other tests comes right after the last interaction
that needs it.
