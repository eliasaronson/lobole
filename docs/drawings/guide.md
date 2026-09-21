# Drawings — workflow guide

How to make a 2D drawing of a part or assembly, in the GUI and over the
RPC/MCP interface. For the design rationale see [plan.md](plan.md).

A **drawing** is a third document kind (alongside parts and assemblies): one
sheet holding hidden-line-removed **views** projected from a source part or
assembly, plus driven **dimensions** measured off the projected geometry.
Projected geometry and measured values are *derived* — they are recomputed on
demand, never stored — so a drawing file is just the setup (sheet, view list,
dimension list).

## Concepts

- **View** — a projection of a source document in a named orientation
  (`front` / `back` / `left` / `right` / `top` / `bottom` / `iso`), placed on
  the sheet at a scale and position. Visible edges draw solid, hidden edges
  dashed. A **projected child** view folds from a base view and stays aligned
  to it along the shared sheet axis (a `top` child sits directly below its
  `front` base; a `right` child beside it).
- **Dimension** — a *driven* (read-only) value measured from projected
  geometry: `distance` / `distance_x` / `distance_y` between two edges,
  `radius` / `diameter` of a circle, or `angle` between two edges. Editing
  geometry happens in the part — a drawing measures, it does not drive.
  A `diameter` dim also carries the band the MODEL records for that bore
  (`Ø20 H7`, or `Ø20 +0.05/-0.02` for an explicit one), read from the part's
  tolerance rather than authored on the sheet — so it follows an edit and
  needs no field of its own. A `radius` dim does not: an ISO class states how
  a DIAMETER may vary. See [../tolerances/plan.md](../tolerances/plan.md).
- **Explicit update** — projection is hidden-line removal, which is expensive,
  so it never runs automatically. A view whose source changed reads **stale**
  until you update the drawing. (The first view of a drawing tab, and an
  opened drawing, project once automatically.)
- **Note** — a line of text placed on the sheet, optionally with a leader to
  a piece of projected geometry. The annotation class no dimension kind
  covers ("6× Ø4.4 on Ø175.8 PCD", "break all sharp edges").
- **Configuration** — a view of a PART can name one of its configurations and
  draws the part in that variant, whatever the model tab has active. That is
  how a sheet shows a sheet-metal part's `Flat` blank beside its folded
  views. Left unset, a view draws whatever the part has active.
- **Bend line** — a flat sheet-metal blank's crease, drawn as a chain line
  (long dash, dot) in a view that looks square onto the blank. It comes from
  the bend the feature recorded, not from projected geometry, so it cannot be
  dimensioned to yet.
- **Bend table** — a note whose rows are its view's part's bends: angle,
  inside radius, K, the allowance a flat crease eats, and UP or DOWN as seen
  from that view (edge-on, a crease has neither). UP is a question about a
  SIDE, so a bottom view calls the same crease DOWN — and the DXF, which is
  read from the side the part's first flange was picked on, can label it
  differently again. Regenerated from the part
  at every resolve; its own text is only the caption. Rows are numbered in
  the part's body order, and the bend lines are not labelled with those
  numbers yet.
- **Centre mark** — the little cross at the centre of every visible circle,
  drawn automatically (sized from the circle) in the GUI, the SVG and the PDF.
  Nothing to add and nothing to place.
- **Dangling** — a dimension whose referenced edge was consumed upstream (a
  fillet ate it, a feature was deleted) resolves as dangling and is flagged;
  it is never silently rebound to a different edge.

## In the GUI

1. **`+Dwg`** on the tab bar adds a drawing tab. The Model panel becomes the
   drawing panel, listing the sheet, the views and the dimensions; the
   viewport becomes a 2D sheet (scroll to zoom about the cursor, middle-drag
   to pan, the toolbar's **Fit** to recentre). The drawing toolbar holds the
   verbs: Add view, Update views, the dimension tools, Fit and Export.
2. **Sheet** — pick the paper size (A4–A0 / Letter), orientation, and fill the
   **Title block** fields (project / document / author / date). A fifth row,
   UNITS, is filled in for you and cannot be typed: it states what the sheet's
   dimensions are in.

   Dimension text is in the SOURCE document's units, not the reader's — a
   drawing of an inch part is an inch drawing wherever it is opened. Lengths
   carry no suffix, as drafting practice wants, which is exactly why the UNITS
   row exists; angles keep their `°` (or three decimals and `rad` on a
   document that asked for radians).
3. **Views** — the toolbar's **Add view** opens a tool window: choose a
   source (part or assembly), the orientation (any of the seven), a scale (0
   picks a standard one that fits), and, for a part with configurations,
   which **Configuration** to draw, then **Add view**. Each view is a row in
   the panel, with its orientation and scale; right-click it to change the
   **Scale**, add a **Bend table** beside a view of a part with bends, or
   **Delete** it. Set a view's `base_view` to fold it from another. The
   toolbar's **Update views** (re)projects; the panel says when views are
   stale.
4. **Dimensions** — click a dimension tool on the toolbar (Distance, Dist X/Y,
   Radius, Diameter, Angle, Hole callout, Pattern callout, Part note), then
   click the projected edges/circles on the sheet; the dimension appears once
   enough are picked. It **places itself** clear of the views, the title block
   and the labels already on the sheet; drag its value label to move it. Each
   dimension is a row in the panel, with its value; right-click to delete it.
5. **File → Export…** writes the sheet as **SVG** or **PDF**.

Notes are drawn on the sheet (and exported) but are **added over the RPC**
today — `add_drawing_note` below; a view row's Add bend table is the only
note the GUI adds.

## Over RPC / MCP

The same workflow headless (snake_case RPCs; the MCP tools mirror them 1:1):

```
create_drawing {name?, sheet?: {size, landscape, title_block:{...}}}
add_drawing_view {source, orientation?, scale?, position?, base_view?,
                  configuration?}   # a part's configuration, by name or id
update_drawing            # the explicit HLR re-projection; returns per-view counts
list_drawing_geometry {view}   # discover a view's edges/circles + the ref to dimension
add_drawing_dim {view, kind, refs:[{edge|sil_face:{feature_id, element},
                                     occurrence?, end?}], offset?}
add_drawing_note {text, position:[x,y], view?, ref?:{feature_id, element},
                  table?: "bends"}  # a bend table of `view`'s part
list_drawing_notes / remove_drawing_note {id}
check_drawing            # views stale/dangling refs/labels that collide
export_drawing {path}     # .svg or .pdf by extension
```

`offset` on a dimension is OPTIONAL: left out, the label is placed outside the
nearest view edge, skipping the other views, the title block and the labels
already placed. `check_drawing` reports what is left overlapping, so a sheet
that needs hand-placing says so instead of exporting unreadable.

`refs` are toponaming references: `list_drawing_geometry` returns, for each
projected edge/circle, the `ref` (`{feature_id, element}`) and its
`source_id` (pass as `occurrence`) to hand straight back to `add_drawing_dim`.
`end` selects a vertex: `0` whole edge (its midpoint), `1`/`2` an endpoint.

End-to-end example (a dimensioned drawing of a part, fully headless):

```python
create_drawing(name="bracket", sheet={"size":"a3","landscape":True,
    "title_block":{"project":"Gearbox","document":"Bracket"}})
front = add_drawing_view(source="bracket-part", orientation="front", position=[130,210])["id"]
add_drawing_view(source="bracket-part", orientation="top", base_view=front, position=[0,120])
update_drawing()
g = list_drawing_geometry(view=front)["geometry"]
vlines = sorted([e for e in g if e["type"]=="line" and abs(e["p1"][1]-e["p2"][1])>5],
                key=lambda e: e["p1"][0])
add_drawing_dim(view=front, kind="distance_x",
    refs=[{"edge":vlines[0]["ref"], "occurrence":vlines[0]["source_id"]},
          {"edge":vlines[-1]["ref"],"occurrence":vlines[-1]["source_id"]}], offset=[0,-14])
export_drawing(path="bracket.pdf")
```

## Not in v1

Section / detail / broken / crop views, balloons + BOM tables on the sheet,
exploded drawing views, multi-sheet drawings, GD&T / weld / surface symbols,
templates/styles, and DXF output are deferred (see plan.md). Driving
dimensions from the sheet is refused by design — a drawing measures; the part
edits.
