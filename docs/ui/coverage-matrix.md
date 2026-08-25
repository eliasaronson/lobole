# Interactive tools x tab kinds — coverage matrix

> **Generated** by `tools/coverage_matrix.py`; do not hand-edit.
> Re-run it after adding GUI tests.



The recurring defect shape in this codebase is not "a feature is untested".
It is "a feature is tested on a part tab and nowhere else", and it has cost
real bugs three times:

  - the section drag arrow was dead on assembly tabs, twice — it was
    published and drawn there, so it even highlighted, but the block routing
    clicks to handles lived only in the part path;
  - F9's entire sharing surface was part-gated and failed on an assembly tab
    (9048d58), found only when F10 went looking at autosave;
  - the drawings round found six RPCs gated by a part-only list.

Asymmetric coverage hides whole-tab-kind failures, because the tests that
exist all pass. The roadmap asks the question this file answers: for each
interactive tool, does a test exercise it on every tab kind it claims to
support?

## The matrix

A cell shows how many tests exercise that tool on that tab kind. **`GAP`** means the tool is meant to work there and nothing tests it. `-` means it is not meant to.

| Tool | part | assembly | drawing |
|---|---|---|---|
| Measure | 10 | 2 (reviewed) | — |
| Section | 5 | 2 (reviewed) | — |
| Mate | (1) | 2 | — |
| Occurrence gizmo | (1) | 4 | — |
| Preview handles | 29 | (1) | — |
| Sketch dimensions | 50 | — | — |
| Sketch drawing | 78 | — | — |
| CAM tool | 9 | — | — |
| Simulation loads | 10 | — | — |
| Feature tree | 38 | 3 _(thin)_ | 2 _(thin)_ |
| Tab bar | 5 | 2 _(thin)_ | 2 _(thin)_ |

A number in brackets is coverage on a tab kind the tool is not listed as supporting — usually a test that touches it incidentally, occasionally a sign the `supports` column is wrong. `(reviewed)` is a cell read by hand and found adequate despite a low count; the reasons are below.

### Reviewed cells

A count cannot tell TARGETED coverage from sparse coverage. These were read and judged fine:

- **Measure** on **assembly** — targeted, not sparse: `whole_bodies_and_parts` asserts the OVERLAY on an assembly tab, which is the exact thing measure-v2 found missing there, and `assembly_distance` pins occurrence-qualified picks. The part side's extra tests are about part-only concerns (corner points, seeds-from-selection, copy-to-variable).
- **Section** on **assembly** — targeted: `offset_arrow_drags_on_an_assembly` IS the regression test for the bug this whole matrix exists because of, and `assembly_cut_is_capped_and_centered` covers the caps. Two tests aimed at the two known failures beat five aimed at nothing.

## Gaps and thin cells

No supported tab kind is completely untested.

**Thin** — covered, but far less than the same tool's best-covered tab kind. This is the shape every historical bug here actually had: some coverage, not enough of it.

- **Feature tree** on **assembly**: 3 test(s) against 38 on its best-covered kind.
- **Feature tree** on **drawing**: 2 test(s) against 38 on its best-covered kind.
- **Tab bar** on **assembly**: 2 test(s) against 5 on its best-covered kind.
- **Tab bar** on **drawing**: 2 test(s) against 5 on its best-covered kind.

## Per-tool detail

### Measure

Supports: part, assembly.

- **part**: `app/new_document_closes_tool_sessions`, `isolation/a_messy_test_leaks_nothing`, `measure/a_corner_is_a_measurable_point`, `measure/cleared_on_mode_entry`, `measure/cleared_on_tab_switch`, `measure/copy_and_make_a_variable`, `measure/hover_previews_before_the_click`, `measure/pin_qualifiers_and_totals`, `measure/seeds_from_selection_and_names`, `measure/touching_leads_with_the_angle`
- **assembly**: `measure/assembly_distance`, `measure/whole_bodies_and_parts`

### Section

Supports: part, assembly.

- **part**: `isolation/a_messy_test_leaks_nothing`, `section/datum_source_and_tilt`, `section/offset_arrow_drags_the_cut`, `view/shadow_toggle_survives_section`, `view/ssao_toggle_survives_section`
- **assembly**: `section/assembly_cut_is_capped_and_centered`, `section/offset_arrow_drags_on_an_assembly`

### Mate

Supports: assembly.

- **part**: `app/new_document_closes_tool_sessions`
- **assembly**: `assembly/mate_edit_repick_connector`, `assembly/mate_hover_marker`

### Occurrence gizmo

Supports: assembly.

- **part**: `handles/hover_highlights_the_arrow`
- **assembly**: `assembly/dragging_a_unit_keeps_its_shape`, `assembly/insert_places_a_sub_assembly`, `handles/gizmo_hover_names_the_axis`, `section/offset_arrow_drags_on_an_assembly`

### Preview handles

Supports: part.

- **part**: `defaults/boolean`, `defaults/box`, `defaults/chamfer`, `defaults/curve3d`, `defaults/cylinder`, `defaults/datum_axis`, `defaults/datum_plane`, `defaults/draft`, `defaults/extrude`, `defaults/fillet`, `defaults/hole`, `defaults/loft` (+17 more)
- **assembly**: `section/offset_arrow_drags_on_an_assembly`

### Sketch dimensions

Supports: part.

- **part**: `app/new_document_closes_tool_sessions`, `defaults/boolean`, `defaults/box`, `defaults/chamfer`, `defaults/curve3d`, `defaults/cylinder`, `defaults/datum_axis`, `defaults/datum_plane`, `defaults/draft`, `defaults/extrude`, `defaults/fillet`, `defaults/hole` (+38 more)

### Sketch drawing

Supports: part.

- **part**: `constraints/coincident_merges_points`, `constraints/horizontal_levels_line`, `datum/plane_offset_then_sketch_on_its_row`, `defaults/boolean`, `defaults/box`, `defaults/chamfer`, `defaults/curve3d`, `defaults/cylinder`, `defaults/datum_axis`, `defaults/datum_plane`, `defaults/draft`, `defaults/extrude` (+66 more)

### CAM tool

Supports: part.

- **part**: `app/new_document_closes_tool_sessions`, `cam/an_operation_can_be_removed_where_made`, `cam/fit_keeps_the_model_clear_of_the_tool`, `cam/look_down_the_spindle`, `cam/the_first_pick_makes_the_setup`, `cam/tool_window_follows_the_viewport`, `cam/verbs_two_clicks_to_a_pocket`, `isolation/a_messy_test_leaks_nothing`, `toolbar/every_tool_is_reachable`

### Simulation loads

Supports: part.

- **part**: `frametime/frame_stats_measure_hud_stays_hidden`, `measure/hover_previews_before_the_click`, `settings/a_restart_row_says_restart`, `sim/a_static_run_matches_beam_theory`, `sim/force_arrow_points_along_the_force`, `sim/force_pushes_into_the_picked_face`, `sim/gravity_pulls_down`, `sim/new_document_leaves_the_mode`, `sim/topology_holds_its_volume_target`, `viewport/shift_scroll_pans`

### Feature tree

Supports: part, assembly, drawing.

- **part**: `autosave/recovers_every_document`, `configs/a_parametric_family_by_clicking`, `conflicts/resolve_a_merge_in_the_panel`, `defaults/boolean`, `defaults/box`, `defaults/chamfer`, `defaults/curve3d`, `defaults/cylinder`, `defaults/datum_axis`, `defaults/datum_plane`, `defaults/draft`, `defaults/extrude` (+26 more)
- **assembly**: `assembly/a_long_row_elides_and_names_itself`, `assembly/an_exploded_view_moves_what_is_drawn`, `assembly/bom_counts_through_a_sub_assembly`
- **drawing**: `drawing/build_a_drawing_by_clicking`, `parity/clicking_and_scripting_agree`

### Tab bar

Supports: part, assembly, drawing.

- **part**: `tabs/add_menu`, `tabs/folder_empty_create`, `tabs/folder_flyout`, `tabs/folder_manage`, `tabs/overflow_scrolls_and_plus_stays`
- **assembly**: `tabs/an_assembly_tab_folders_like_a_part`, `tabs/kind_glyphs_and_stable_ids`
- **drawing**: `drawing/build_a_drawing_by_clicking`, `parity/clicking_and_scripting_agree`
