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
- **Assembly mates, drawing dimensions and the simulation toolbar** have their
  own click-through coverage in `assembly/*` and `drawing/*`, but not
  button-by-button; the mate and dimension tests reach them through the tool
  window rather than every toolbar id.

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
  to. Listing a Loft's sections closed the Loft. It clicks the opener again
  now.
