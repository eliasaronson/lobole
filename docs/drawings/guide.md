# Drawings — workflow guide

How to make a 2D drawing of a part or assembly, in the GUI and over RPC/MCP.
How it works underneath is in [README.md](README.md).

A **drawing** is its own document kind beside parts and assemblies: one sheet
of hidden-line **views** projected from a source part or assembly, plus
driven **dimensions** measured off the projected geometry. Projected geometry
and measured values are recomputed, never stored, so a drawing file is only
its setup (sheet, views, dimensions, notes).

## Concepts

- **View**: a projection of a source document in a named orientation
  (`front` / `back` / `left` / `right` / `top` / `bottom` / `iso`), placed on
  the sheet at a scale and position. Visible edges draw solid, hidden edges
  dashed. A **projected child** folds from a base view and stays aligned to
  it along the shared sheet axis (a `top` child shares its `front` base's X,
  a `right` child its Y).
- **Quality**: `exact` (the default) traces every curve to the model edge
  that made it, which is what dimensions attach to. `fast` is a polygonal
  look at a sheet that would otherwise take long to project; nothing on a
  fast view can be dimensioned.
- **Hidden occurrences**: an assembly view can leave occurrences off. Exact
  projection is priced by surface complexity, so one spring or imported part
  can be most of a view's time; the view's cost report names it.
- **Dimension**: a *driven*, read-only value measured from projected
  geometry: `distance` / `distance_x` / `distance_y` between two edges,
  `radius` / `diameter` of a circle, `angle` between two edges, and callouts
  (`hole_callout` from the hole feature, `pattern_callout` over two or more
  circles, `fastener_callout` naming a standard part on an assembly view).
  Editing geometry happens in the part. A `diameter` also shows the tolerance
  the part records for that bore (`Ø20 H7`, or `Ø20 +0.05/-0.02`); see
  [../tolerances/README.md](../tolerances/README.md).
- **Explicit update**: projection is expensive, so it does not run on every
  change. A view whose source changed reads **stale** until the drawing is
  updated. Entering a drawing tab projects it.
- **Note**: a line of text on the sheet, optionally with a leader to
  projected geometry ("6× Ø4.4 on Ø175.8 PCD", "break all sharp edges").
  `{{expression}}` in the text is replaced by a variable's value.
- **Configuration**: a view of a part can name one of its configurations and
  draws the part in that variant, whatever the model tab has active — a
  sheet-metal part's `Flat` blank beside its folded views. Unset, it draws
  whatever the part has active.
- **Bend line**: a flat blank's crease, drawn as a chain line in a view that
  looks square onto the blank. It comes from the recorded bend, not from
  projected geometry, so it cannot be dimensioned to.
- **Bend table**: a note whose rows are its view's part's bends (angle,
  inside radius, K, bend allowance, and UP or DOWN as seen from that view),
  regenerated from the part at every update. A bottom view calls the same
  crease DOWN that a top view calls UP; the flat-pattern DXF answers from the
  side the first flange was picked on and may differ.
- **Section view**: a view with a `section` point is cut by the plane through
  it square to the view's direction; the cut faces are hatched, the view is
  captioned SECTION A-A, and the view it folds from draws the lettered
  cutting-plane line. Dimension a bore in section with a `diameter` across
  its two walls (one `sil_face` ref: the walls are the bore's face).
- **Parts list and balloons**: a `table: "bom"` note on an assembly view lists
  the parts that view draws (leave the load out with `hidden`); a `balloon`
  note with no text, its leader on an occurrence, shows that part's item
  number from the list.
- **Datum**: a `datum` note — the letter in a box, a triangle on the feature.
- **Thread**: a cosmetic or tapped thread draws in thin line by ISO 6410
  (three quarters of a circle end-on, two lines from the side), and a tapped
  hole's callout names the thread, its class, depth and drill.
- **Centre mark**: drawn automatically on every circle seen whole.
- **Dangling**: a dimension whose edge was consumed upstream (a fillet ate
  it, a feature was deleted) is flagged and left out of the export; it is
  never rebound to a different edge.

## In the GUI

1. **+ → New Drawing** on the tab bar adds a drawing tab. The Model panel
   becomes the drawing panel (Sheet, Title block, Views, Dimensions); the
   viewport becomes the sheet: scroll zooms about the cursor, middle-drag (or
   Alt+left-drag, Shift+scroll) pans, the toolbar's **Fit** recentres. The
   drawing toolbar holds Add view, Update views, the dimension tools, Fit and
   Export.
2. **Sheet**: paper size (A4–A0, Letter), landscape, and the title block
   fields (project, document, author, date). The UNITS row is filled in from
   the source document and cannot be typed. Dimension lengths carry no unit
   suffix, as drafting practice wants, which is why that row exists; angles
   keep their `°`.
3. **Views**: **Add view** opens a tool window: source (part or assembly),
   orientation, scale (0 picks a standard scale that fits), and for a part
   with configurations, which **Configuration** to draw. The view is placed
   clear of the views already there. Each view is a row in the panel;
   right-click it to change the **Scale**, **Add bend table** beside a view
   of a part with bends, or **Delete** it. **Update views** re-projects; the
   panel says when views are stale.
4. **Dimensions**: pick a tool on the toolbar (Distance, Dist X, Dist Y,
   Radius, Diameter, Angle, Hole callout, Pattern callout — Enter finishes
   the pick — or Part note), then click projected edges or circles on the
   sheet. The dimension places itself clear of the views, the title block and
   other labels; drag its value label to move it. Right-click a dimension's
   row to delete it.
5. **Export** writes the sheet as SVG or PDF. It needs a system file dialog;
   without one, use `export_drawing`.

Folding a child from a base view, hiding occurrences, choosing `fast`, and
free notes are set over RPC.

## Over RPC / MCP

```
create_drawing {name?, sheet?: {size, landscape, title_block: {project, document,
                author, date, notes, material, scale}}}
set_drawing_sheet {size?, landscape?, title_block?}
add_drawing_view {source, orientation?, scale?, position?, base_view?,
                  configuration?, quality?: "exact"|"fast", hidden?: [occurrence ids or names],
                  rotation_deg?, section?: {point: [x, y, z], label?}}
set_drawing_view {id, position?, scale?, name?, show_hidden?, quality?,
                  base_view?, configuration?, hidden?, rotation_deg?, section?}
update_drawing {budget_s?}     # re-project; per-view curve counts and errors
list_drawing_views             # placement, stale, and what each projection cost
list_drawing_geometry {view}   # a view's curves, with the ref to dimension each
add_drawing_dim {view, kind, refs: [{edge | sil_face: {feature_id, element},
                                     occurrence?, end? | extreme?}], offset?}
list_drawing_dims / remove_drawing_dim {id}
add_drawing_note {text, position: [x, y], view?, ref?: {feature_id, element}
                  | {edge | sil_face: {...}, occurrence?, end? | extreme?},
                  style?: "text"|"balloon"|"datum", table?: "bends"|"bom"}
list_drawing_notes / remove_drawing_note {id}
check_drawing {drawing?}       # stale views, dangling refs, overlapping labels
export_drawing {path}          # .svg or .pdf
```

`source` is the source document's name. Omit both `scale` and `position` on
`add_drawing_view` and it is placed the way the Add view button places it,
projected, and the response carries its cost. `offset` on a dimension is in
sheet millimetres and optional; left out, the label is placed clear of the
views, the title block and existing labels. `check_drawing` reports anything
left overlapping.

`list_drawing_geometry` returns, for each projected edge or circle, its `ref`
(`{feature_id, element}`) and `source_id` (pass as `occurrence`) to hand
straight to `add_drawing_dim`, and each curve's `bbox`. `end` selects a
vertex: `0` the whole edge (a circle's centre, anything else's midpoint),
`1`/`2` an endpoint. `extreme` (`left`, `right`, `top`, `bottom`) is instead
the curve's furthest point that way on the sheet -- the top of a turned
piece, which is neither end of its outline. `rotation_deg` turns a view on
the sheet ([rotation](README.md#rotation)).

A dimensioned drawing of a part, headless:

```python
create_drawing(name="bracket", sheet={"size": "a3", "landscape": True,
    "title_block": {"project": "Gearbox", "document": "Bracket"}})
front = add_drawing_view(source="bracket-part", orientation="front",
                         position=[130, 210])["id"]
add_drawing_view(source="bracket-part", orientation="top", base_view=front,
                 position=[0, 120])
update_drawing()
g = list_drawing_geometry(view=front)["geometry"]
vlines = sorted([e for e in g if e["type"] == "line"
                 and abs(e["p1"][1] - e["p2"][1]) > 5],
                key=lambda e: e["p1"][0])
add_drawing_dim(view=front, kind="distance_x",
    refs=[{"edge": vlines[0]["ref"], "occurrence": vlines[0]["source_id"]},
          {"edge": vlines[-1]["ref"], "occurrence": vlines[-1]["source_id"]}],
    offset=[0, -14])
export_drawing(path="bracket.pdf")
```

A slow assembly view: read `list_drawing_views` → `cost.by_occurrence`, then
either `set_drawing_view {id, hidden: [...]}` to leave the dear occupant off,
or `quality: "fast"` for a look. `update_drawing {budget_s}` bounds how long
an update may run.

What drawings do not do yet is listed in [README.md](README.md#not-done).
