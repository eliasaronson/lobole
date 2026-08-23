# Which features are actually driven through the GUI (2026-08-14)

Every feature in this app has two front doors: the RPC surface that scripts and
tests use, and the buttons a person clicks. The first has always been well
covered — hundreds of headless tests build features by posting JSON. The
second was covered by *assertion*, and the difference matters: a tool whose
parameters, pick slots and Accept path are never exercised can be broken for
weeks while its feature type passes every unit test. The U9 and U10 rounds
each found tools in exactly that state.

So this is the honest table. "GUI" means a test clicks the toolbar button,
fills the tool window, and accepts — and then checks the *geometry*, not just
that a feature appeared.

| Feature | Toolbar | GUI test |
|---|---|---|
| Box / Cylinder / Sphere | `###tb_box` … | `document/create_sphere_and_cylinder`, and 39 tests build a box |
| Sketch | `###tb_new_sketch` | `sketch/*` (many), plane picks from the tree and the datum row |
| Extrude | `###tb_extrude`, `###tb_sk_extrude` | `sketch/model_toolbar_extrudes_finished`, `handles/extrude_arrow_drag_sets_distance` |
| Revolve | `###tb_revolve` | `revolve/profile_beside_the_axis_rings` (volume checked against Pappus) |
| Sweep | `###tb_sweep` | `sweep/profile_along_a_path_makes_a_bar` |
| Loft | `###tb_loft` | `loft/two_profiles_via_the_section_combo` |
| Pipe / Curve3D | `###tb_pipe`, `###tb_curve3d` | `curves/pipe_along_helix_makes_body`, `curves/create_helix_from_menu` |
| Fillet / Chamfer | `###tb_fillet`, `###tb_chamfer` | `fillet/fillet_body_edge_via_click`, `chamfer/edge_via_click_makes_a_chamfer` |
| Hole | `###tb_hole`, `###tb_sk_hole` | six `hole/*` cases (pick, preselect, Enter, Escape, curved-face refusal) |
| Shell | `###tb_shell` | `shell/open_face_pick_hollows_the_box` |
| Draft | `###tb_draft` | `draft/face_pick_names_the_body` |
| Split / Refine | `###tb_split`, `###tb_refine` | `split/default_plane_cuts_the_body_in_two`, `refine/merges_the_seam_left_by_a_union` |
| Rib | `###tb_rib` | `rib/menu_rib_previews_and_adds_volume`, and the defaults gate |
| Thread | `###tb_thread` | `thread/face_pick_fills_the_slot` |
| Boolean | `###tb_boolean` | three tests |
| Pattern / Mirror | `###tb_pattern`, `###tb_mirror` | `pattern/circular_preview_param_edits_survive`, defaults gate |
| Datum plane / axis | `###tb_datum_plane`, `###tb_datum_axis` | `datum/plane_offset_then_sketch_on_its_row`, defaults gate |

Plus the `defaults/*` gate (`docs/ui/u10-defaults-and-refusals.md`), which opens
**every** model tool with nothing selected and asserts it does something
defensible — that is the net that catches a new tool born broken.

## Still not driven by a click, and why

- **Import** (`###tb_insert_part`, Ctrl+I). The first thing it does is open the
  desktop's file dialog, which is a portal process outside ImGui and outside
  the test engine. The importer itself is covered headlessly
  (`test_step_import`, `remote_smoke`), so what is untested is the dialog
  round trip — the one part of it we do not own.
- **The sketch toolbar's Revolve** (`###tb_sk_revolve`) is a second button onto
  the same action as `###tb_revolve`, which is tested; the button itself is
  not.
- **Reference images** (`###tb_sk_image`) and the two display toggles
  (`###tb_sk_autocon`, `###tb_sk_glyphs`).
- **Assembly mates and drawing dimensions** have their own click-through
  coverage in `assembly/*` and `drawing/*`, but not button-by-button; those
  tests reach the tools through the tool window rather than every toolbar id.
- **The simulation toolbar** is covered by `sim/*` since U11
  (docs/ui/u11-simulation-setup.md): entering the mode, the material fix, a
  force placed by clicking a face, gravity, and leaving the mode on File >
  New. Pressure and Fixed are placed but not asserted to the same depth.

## The trap this round turned up

`LabeledCombo` used to name its widget `"##" + label`. Both `##` and `###`
hide the label; the difference is that `##` leaves the widget with **no name
at all**, so it can only be addressed by the numeric id `ui_items` reports.
Every combo in the app was unreachable by name — which cost an afternoon
twice: once driving the Hole type combo over the RPC, once writing the Loft
test. It is `"###" + label` now: same appearance, and `//###ToolWindow/###Add`
resolves.

Two related gotchas, since they cost time too:

- A **combo popup does not take window focus**. `//$FOCUSED/Item` does not
  find it — address it by the popup's own window name, the way
  `RemoteServer`'s `popup_click` does.
- `popup_items` used to close the popup with **Escape**, which falls straight
  through to the app, where Escape cancels the tool session the popup belongs
  to. Listing a Loft's sections closed the Loft. Clicking the opener again is
  no fix either — a combo popup is drawn OVER its opener, so the click lands
  on the popup (found in U11). It closes the stack directly now.
- An **`ImGui::PushID` scope** has the same effect as a hidden label: the
  widget inside it can only be reached by numeric id. Give a repeated widget a
  unique name (`"Material###material_<feature>"`) instead of wrapping it.
- **A bare `ImGui::Combo("Label", ...)` gathers with an EMPTY label**, not
  with "Label" — so it is unreachable by name even though its label is
  visible on screen, and `ui_items` reports it as `''`. This is why the whole
  drawing panel had no clicked coverage until 2026-08-23: its Source,
  Orientation and Sheet-size combos could not be addressed at all, and a
  Z-up orientation table survived in a Y-up app behind them. Same fix as
  everywhere else — `"Label###stable_id"`.
- **A `##`-hidden TABLE is an unaddressable ID scope**, so every cell inside
  it needs a `**` wildcard — and a wildcard crawl over a table can run past
  the test engine's 300 s watchdog. Name tables too: `BeginTable("###vars_
  table", ...)` makes `//Variables/###vars_table/###var_expr_project_len`
  resolve directly.
- **`PushID(int)` vs `PushID(const char*)`**: a string scope IS addressable
  as a path segment (`//###ToolWindow/Distance/##expr` works), an integer one
  is not. The Variables and Configurations panels used integer scopes, which
  is why neither had ever been clicked by a test.
- **`ComboClick` wants a ref RELATIVE to the window.** `SetRef("//Model")`
  then `ComboClick("###dwg_orientation/front")`; the window-qualified form
  fails with `popup && IsWindowACombo(popup)`, which does not sound like an
  addressing problem.
- **A widget scrolled out of a panel reports an EMPTY label**, so anything
  below the fold is unreachable by name — and there is no RPC that scrolls a
  panel, so an agent cannot bring it into view either (the dock round's open
  P5 papercut). `ui_items` still LISTS the widget, with its position and its
  numeric id, which is what makes this one confusing: the row is there, the
  name is not. Found 2026-08-23 in the simulation panel, where **Run** is the
  control that drops below the fold the moment you arm a BC tool — the reason
  no test had ever run a study by clicking. A `###id` fixes it, because
  `ItemInfo` resolves a path by HASH and never consults the label: with one,
  `//###Model/###study_analysis` clicks a combo that is scrolled off the top
  of the panel. Verified both ways before and after.
- **`SameLine()` never wraps.** Three buttons in a 210 px dock draw the third
  one past the panel edge, where it cannot be clicked or seen: the simulation
  panel lost the Pressure boundary condition and the Displacement result
  field that way, both of them the last item of a row. `ui::WrapRow`
  (`ui/PanelLayout.h`) is the shared fix — call `Next(ButtonWidth(label))`
  before each widget and it breaks the line when one will not fit.


## The trap 2026-08-22 turned up: a failed lookup poisons the whole test

`ImGuiTestContext::WindowInfo` and `ItemInfo` **log an error when they do not
find their target**, and the context then short-circuits every later lookup:

```cpp
ImGuiTestItemInfo ImGuiTestContext::WindowInfo(ImGuiTestRef ref, ...)
{
    if (IsError())
        return ItemInfoNull();      // imgui_te_context.cpp:1095
```

So a test that asserts something is **absent** —

```cpp
IM_CHECK(ctx->WindowInfo("//Terminal###TerminalPanel").Window == nullptr);
```

— *passes*, because null is what it wanted, and from that point on every
`WindowInfo`/`ItemInfo` in that test returns null no matter what the UI is
doing. Ask with `ImGuiTestOpFlags_NoError` when "no" is a legitimate answer.
`ItemExists` is already safe: it passes the flag itself.

**Why it hides so well.** Run alone, the window has never existed and that
path does not log, so the test is green. Run after a test that created the
window, a different path inside `WindowInfo` runs — the one that logs — and
the test fails *later*, at an assertion that looks unrelated. The default
4-shard partition can separate the two tests, so the suite is green; the
partition is balanced by recorded times, so **adding tests reshuffles it** and
the failure appears and disappears for reasons that have nothing to do with
the code under test.

This cost two sessions about ten hypotheses between them — docking, tab
selection, PTY spawn timing, stale `DockId` after a layout rebuild, load — and
none of them was it. **`--serial` is what makes it findable**, because it puts
every test in one process; a green sharded run is not evidence.

Fixed in `terminal/*` and `perf/*` (`3dbc0c0`, `6cca9bf`). If you write a test
that checks a window, panel or dialog is *not* there, use `NoError`.
