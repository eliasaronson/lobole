# Drawings — workflow guide

How to make a 2D drawing of a part or assembly, in the GUI and over RPC/MCP.
How it works underneath is in [README.md](README.md).

A **drawing** is its own document kind beside parts and assemblies: one sheet
of hidden-line **views** projected from a source part or assembly, plus
driven **dimensions** measured off the projected geometry. Measured values
are recomputed, never stored, so a drawing file is its setup (sheet, views,
dimensions, notes) plus, in the directory form, each view's last projection
as a cache ([saved projections](README.md#saved-projections)).

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
- **Hidden occurrences and bodies**: an assembly view can leave occurrences
  off, and a part view bodies, by body name or creating-feature id
  ([leaving bodies off a view](README.md#leaving-bodies-off-a-view)). Exact
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
  updated. Entering a drawing tab for the first time asks for its
  projection, which runs off the frame: the views land one at a time, a
  view not yet projected is a dashed placeholder at its source's bounds, and
  the panel shows the progress with Cancel
  ([projection off the frame](README.md#projection-off-the-frame)).
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
- **Detail view**: a circle on another view drawn again at a larger scale,
  lettered on its parent and captioned DETAIL B (5:1) under it. It draws
  whatever its parent draws (a section too), and follows the parent.
- **Parts list and balloons**: a `table: "bom"` note on an assembly view lists
  the parts that view draws (leave the load out with `hidden`), each with
  its material's grade (6082-T6); a `balloon` note with no text, its leader
  on an occurrence, shows that part's item number from the list.
  `layout_balloons` puts one on every row for you, round the views you name,
  with no two leaders crossing.
- **Datum**: a `datum` note — the letter in a box, a triangle on the
  feature. It is a datum FEATURE: a feature control frame names it by its
  letter, so each letter is used once on a sheet (and never I, O or Q).
- **Feature control frame**: a `style: "fcf"` note — the ISO 1101 symbol,
  the tolerance zone (Ø for a cylindrical one, Ⓜ/Ⓛ for a material
  condition) and the datums it is measured from, with a leader to the
  feature or stacked with a dimension's value. What the symbol cannot
  carry (a datum on a flatness, no datum on a perpendicularity) is refused.
- **Aligned values**: a length is written along its dimension line, above
  it, readable from the bottom or the right of the sheet; a vertical one is
  turned to read upward.
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
   of a part with bends, or **Delete** it. **Update views** re-projects the
   views that need it, off the frame, with its progress and Cancel on the
   panel; the panel says when views are stale. File > Export writes once the
   views have landed.
4. **Dimensions**: pick a tool on the toolbar (Distance, Dist X, Dist Y,
   Radius, Diameter, Angle, Hole callout, Pattern callout — Enter finishes
   the pick — or Part note), then click projected edges or circles on the
   sheet. The dimension places itself clear of the views, the title block and
   other labels; drag its value label to move it. Right-click a dimension's
   row to delete it.
5. **Datums and frames**: **Datum** (`Shift+D`) and **Feature control
   frame** (`G`) on the toolbar each open a session in the tool window. Click
   the feature's edge on the sheet; the symbol then stands clear of the view,
   outward from the edge, and dragging it moves it (clicking another edge
   moves its leader). A datum takes the sheet's next free letter. A frame
   starts as flatness on a straight edge and as a Ø position on a circle, its
   zone ISO 2768-2 class K for the feature's length (perpendicularity's
   table for an orientation tolerance), until you choose or type otherwise;
   the window has the symbol, the zone, Ø, Ⓜ/Ⓛ, and the datums the sheet
   has, primary first. A frame can instead stand under a dimension's value:
   click the value, or choose **Feature control frame** from the
   dimension's row menu. What the sheet cannot take — a datum letter used
   twice or that is I, O or Q, a flatness naming a datum, a perpendicularity
   naming none — is said in the window in the words `add_drawing_note`
   refuses with, and Accept stays off until it is fixed.
6. **Detail views**: **Detail view** (`Shift+I`, or **Detail view** in a
   view row's menu): press on a view at the circle's centre and drag out its
   radius (a click without a drag takes an eighth of the view). The detail
   comes up at the largest standard scale, at least twice its parent's,
   that keeps it within a third of the sheet, lettered with the next free
   letter and placed in the free spot nearest the circle, drawn in preview
   as it will look. Drag it to move it; the window edits radius, scale and
   letter. Accept adds it as one undo step.
7. **Balloons**: **Lay out balloons** (`B`) puts a balloon on every row of
   the parts list round the views of the sheet's assembly; **Lay out
   balloons** in an assembly view's row menu does it round that view. Run
   again, it replaces the balloons rather than adding more. The panel says
   how many it placed, and names any leaders crossing or rows with no clear
   edge.
8. **Export** writes the sheet as SVG or PDF. It needs a system file dialog;
   without one, use `export_drawing`.

Folding a child from a base view, hiding occurrences or bodies, choosing `fast`, a
section view, and free text notes are set over RPC.

## Over RPC / MCP

```
create_drawing {name?, sheet?: {size, landscape, title_block: {project, document,
                author, date, notes, material, scale}}}
set_drawing_sheet {size?, landscape?, title_block?}
add_drawing_view {source, orientation?, scale?, position?, base_view?,
                  configuration?, quality?: "exact"|"fast",
                  hidden?: [occurrence ids or names | body names or feature ids],
                  rotation_deg?, section?: {point: [x, y, z], label?}}
add_drawing_view {detail: {view, center: [x, y, z], radius_mm, label?},
                  scale, position, name?}   # a detail of another view
set_drawing_view {id, position?, scale?, name?, show_hidden?, quality?,
                  base_view?, configuration?, hidden?, rotation_deg?, section?}
update_drawing {budget_s?, async?}  # re-project; per-view curve counts and errors
                               # (waits off the frame; async answers at once)
list_drawing_views             # placement, stale, and what each projection cost
list_drawing_geometry {view}   # a view's curves, with the ref to dimension each
add_drawing_dim {view, kind, refs: [{edge | sil_face: {feature_id, element},
                                     occurrence?, end? | extreme?}], offset?}
list_drawing_dims / remove_drawing_dim {id}
add_drawing_note {text, position: [x, y], view?, ref?: {feature_id, element}
                  | {edge | sil_face: {...}, occurrence?, end? | extreme?},
                  style?: "text"|"balloon"|"datum"|"fcf", table?: "bends"|"bom",
                  fcf?: {symbol, tolerance_mm, diameter?, modifier?: "mmc"|"lmc",
                         datums?: ["A", "B"]}, dim?}
layout_balloons {views: [ids]} # a balloon on every parts-list row, none crossing
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

A frame on a bearing seat, perpendicular to datum A, and the holes'
position under their callout:

```python
add_drawing_note(view=section, style="datum", text="A", position=[60, 50],
                 ref={"sil_face": bottom_face, "extreme": "left"})
add_drawing_note(view=section, style="fcf", position=[230, 120],
                 ref={"sil_face": seat_wall, "end": 1},
                 fcf={"symbol": "perpendicularity", "tolerance_mm": 0.02,
                      "diameter": True, "datums": ["A"]})
add_drawing_note(style="fcf", dim=pattern_callout_id,
                 fcf={"symbol": "position", "tolerance_mm": 0.2,
                      "diameter": True, "datums": ["A", "B"]})
```

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
