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
3. **Parts** — the project's parts, added with the same buttons the Parts
   section has.
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

## What the viewport draws

Show only the consequence of the decision being made, and drop it when the
decision is made.

- **Always, on a job tab:** the parts laid out in setup space; the stock as a
  translucent box; the machine's work area as a dimmer box (red when the job
  does not fit it; never resized); the fixtures, ghosted grey (outside the
  document's body list, so picking and selection indices are untouched); the
  backplot of the generated toolpaths — cuts amber, plunges red, rapids muted
  grey — handed to the renderer's cached channel
  (`SketchRenderer::SetCachedPrimitives`, a shared immutable list uploaded
  only when it changes, never copied per frame).
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
tool.

- **Setup row:** the setup combo, `+` (new setup), **Flip** (the other side,
  derived), `x` (delete). The machine combo, and under it the reconciliation
  line when the embedded machine differs from yours or is not one of yours
  (*Use mine*, *Save as mine* / *Save it*; [setup.md](setup.md#machine-profiles)).
- **Parts:** each job part with its verbs on the name line (Remove, or Place
  for a part without a placement) and its X, Y and turn on the next line,
  named inside the fields; a part whose source is gone says so. **+ Add part**
  opens a menu of the project parts the job does not hold (disabled, saying
  why, when there are none).
- **Fixtures:** one row per fixture of the active setup — a glyph, its name,
  where it sits as a dim tail ("−X side", "over the −X edge", or "its seat is
  gone: …"), greyed when not checked; right-click Edit, Check, Move to (−X, +X,
  −Y, +Y, Centre, Top) and Remove. **+ Add fixture** offers the keep-out box
  first, then "Hold it with a part:" and the project parts the job does not
  cut; each item's tooltip says what the choice does, so cutting a part and
  clamping with it are not confused. An empty section says that verify checks
  no clamps.
- **Stock and safety:** safe Z; the stock mode (Around the part / A fixed
  sheet) with margins or size and place; where zero is (Stock corner, Stock
  centre, Part corner) with the resulting origin printed beside it; and the
  job's copy of the machine's post, arcs, 4th axis and A index.
- **Tools:** rows in the list style (right-click Edit), **Add tool** offering
  the machine's library first. Feeds and speeds are asked for, and the panel
  says why: they are the one number that is genuinely the user's.
- **Operations:** rows in the feature list's style, reorderable by the grip
  (the order is the cutting sequence); double-click opens the op's settings in
  the CAM tool window; right-click Edit, Enable/Disable, Show what it removes,
  Delete. The settings show only the fields the type has (the capability
  table): tool as `T1 T2 …` buttons, depths, stepdown/stepover, leave, tabs,
  corner relief, finish tool, Over openings, the rotary pattern as a radio
  pair, and so on. **Add operation** adds what the setup calls for
  (`cam::AddOperationType`: another rotary pass on a setup that already cuts
  on the 4th axis, else the whole-part cut-out) through `DeriveOperation`, and
  shows a refusal instead of adding a dead operation.
- A one-line **status** that opens the Generate window.

Combos with an empty label cannot be addressed by the GUI test engine, so
choices a test must make (add menus, the rotary pattern, tool buttons) are
popups, buttons or radio pairs. No two visible widgets may share an ImGui id:
ImGui reports a collision in a popup no test sees, so
`cam/panel_widgets_have_distinct_ids` draws the whole panel and fails on one.

## Generate window

The toolbar's Generate (and the panel's status line) opens a tool window with the run and what it
found: generate and verify, the verdict sentences (Good/Warn/Bad, the same
text `cam_verify` returns as `says`) with the cutter findings and machine
lines, the output options — cutting order (`Cut`: authored/tool/part) and
**One file per tool** — **Show path**, and **Save G-code**
([post.md](post.md#files)). A generate on a job tab defers the slow passes to
the background ([README.md](README.md#background-work)); the toolpaths fill in
as they land and a progress row shows the batch. The "N mm of cutting" total is
cached on the toolpaths' key.

## Simulate

`job.simulate` opens the transport window along the bottom of the viewport
([simulation.md](simulation.md#transport)). With nothing generated it refuses
in a popup at the click.

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

The machine store's editor: New / Duplicate / Delete, a list, and an editor
whose every edit writes the file (a rename moves it): name, work area X, Y, Z
(a partly typed area is kept), post, arcs, 4th axis, rapid feed, the A axis's
top speed, and the tool library. Numeric fields go through `ui::ExprInput`.

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
`cam/the_rotary_verbs_rough_then_finish`, `cam/a_rotary_pass_limits_to_a_face`,
`cam/backplot_is_not_copied_per_frame`, `cam/a_job_edit_undoes`. A test that
leaves a CAM window open, or a docked tab switched, breaks later tests' clicks,
so cleanup that matters to other tests comes right after the last interaction
that needs it.
