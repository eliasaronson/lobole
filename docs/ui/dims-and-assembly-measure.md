# Dimension visuals + assembly measure

Status: SIGNED OFF 2026-07-19 (both parts, B-then-A order). Part B (assembly
measure) SHIPPED; Part A (dimension visuals) in progress.
Scope: the two remaining items from the July request batch — CAD-style sketch
dimension indicators (item 1) and measuring inside assemblies (item 11).

---

## Part A — Sketch dimension visuals

### Where we actually are

`DrawDimensions` (SketchInteraction.cpp) is further along than "text labels":
distance dims already draw extension lines, a dimension line with arrowheads
at both ends, and a value with a backing box; double-click opens the inline
value/expression editor. What makes it feel un-CAD-like:

- **Nothing ever sets `labelOffset`.** The field is in the format (serialized
  since v1 of dims, no bump needed) and the renderer honors it — but no UI
  writes it, so every dimension sits at offset 0, glued to (and overlapping)
  its geometry. Drawings solved this already: `DrawingPanel.cpp` drags
  `dim.labelOffset` with the mouse — the exact pattern to port.
- **Angle dims have no geometry at all** — just `123.4°` floating near a
  point. Real CAD draws an arc between the two lines with arrowheads.
- **Radius/diameter leaders have no arrowhead** at the rim and no landing
  line under the value.
- The value box is functional but plain; no hover affordance hinting that it
  is draggable/double-clickable.

### Design

All rendering-only (plus writing the already-serialized `labelOffset`) — **no
format bump**.

1. **Label drag** — the value pill becomes a drag handle: hover highlights it,
   left-drag moves the label in sketch UV, writing `con.labelOffset` (commit
   through the sketch edit path so undo works — one Modify on mouse-release,
   not per-frame). Distance dims: offset moves the whole dimension line away
   from the measured points (extension lines stretch), exactly like today's
   math already computes. Radius/angle: offset moves the label along/around
   the leader/arc.
2. **Angle dims drawn properly**: find the two lines' intersection (or the
   shared endpoint), draw the arc from line A's direction to line B's at
   radius `|labelOffset| (min-clamped)`, arrowheads at both arc ends, short
   extension lines when the picked lines don't reach the arc. Label sits on
   the arc midpoint.
3. **Radial dims**: arrowhead at the rim pointing at the circle (outward when
   the label is outside), short horizontal landing line under the value,
   `R`/`Ø` prefix as today. Label placement via drag anywhere; leader follows.
4. **Fit handling**: when the pill + two arrowheads don't fit between the
   extension lines (short distances), flip the arrows to the outside — the
   standard drafting fallback. Break the dimension line under the pill so the
   number never sits on a line.
5. **Styling**: pill = rounded rect in the theme's accent, brighter on hover,
   the selected/editing dim tinted; consistent between sketch and (later)
   drawings.
6. **Placement-on-create**: creating a dimension seeds a sensible non-zero
   `labelOffset` (offset perpendicular to the measured span, drawings-style
   default) so new dims are born legible instead of on top of the geometry.

Out of scope: click-to-place-before-create flow (SolidWorks' floating dim on
the cursor), tolerances, dual units, ordinate dims. Existing double-click
editor is kept as-is.

### Tests

- GUI: create a distance dim, drag its pill, assert `labelOffset` changed and
  survives save/reload (already-serialized field). An angle-dim case asserting
  it no longer renders as bare text is visual — covered by screenshot review
  on dev-watch rather than an assert.
- No golden bump (no format change).

---

## Part B — Assembly measure

### Where we actually are

Measure is part-only in exactly three places; everything else is ready:

- `MeasureState::Element` = `{ElementRef, isEdge}` — no occurrence identity.
- `Candidates()` face/edge resolution uses `FaceElementRef`/`EdgeElementRef`,
  both of which read `state.document` (null on an assembly tab), so an armed
  measure tool yields no candidates there. The pipeline itself already passes
  armed kinds through on assembly tabs.
- `Measure::Describe/DistanceDetailed` take one part's bodies — no transforms.

Already in place from A2/A3: `PickAssemblyFace` (meshId → occurrence → part →
body → toponame), `OccurrenceMatrix` (rigid world transform), `PartDocById`,
per-instance face-highlight projection (U1b/U5), and world-space
`edgePolylines` on assembly render meshes.

### Design

Session-state only — **no format change**.

1. **Element identity grows an occurrence**: `MeasureState::Element` gains
   `uint64_t occurrenceId` (0 = part context). Toggle/dedup compares it too —
   the same face on two occurrences of the same part is two distinct
   measurables (that's the point of assembly measure).
2. **Assembly candidates**: in `Candidates()`, when `state.assembly` is set
   and Face/Edge kinds are armed, resolve via assembly-aware refs:
   - Face: the `PickAssemblyFace` resolution chain (minus connector snap),
     yielding the ElementRef against the *part's* body + the occurrenceId
     (carried in `SelectionItem.id`).
   - Edge: same meshId → occurrence → part-body walk, then `EdgeNameOf`
     (new small helper next to `EdgeElementRef`; its "part-only" early-out
     stays for the part path).
   `ConsumeMeasure`/`MeasureElementClicked` pass the occurrenceId through.
3. **Core distance with transforms**: new overload
   `Measure::DistanceDetailed(bodiesA, refA, xfA, bodiesB, refB, xfB)` —
   resolve each shape as today, apply the rigid occurrence transform via
   `gp_Trsf`/`TopoDS_Shape::Moved`, extrema in world space, return world
   closest points (feeds the existing orange line + dx/dy/dz unchanged). The
   part path becomes identity-transform calls into the same impl.
   `Describe` needs **no** transform work — area/length/radius are rigid-
   invariant, resolve against the part's bodies directly.
4. **Readout**: `UpdateMeasureReadout` resolves each element's bodies via
   occurrence → `PartDocById`; single-element readouts get the occurrence
   name prefix ("Bracket-1: face, area …"). Cross-occurrence, same-occurrence,
   and mixed measurement all fall out of the per-element resolution.
5. **Highlights**: measured-face projection in `SyncSelectionHighlights`
   finds the render mesh matching (occurrenceId, bodyIndex) and uses the
   existing per-instance highlight; measured-edge primitives already read
   world-space `edgePolylines` — just select the mesh by occurrence.
6. **Entry points**: Measure button on `DrawAssemblyToolbar` (same toggle
   semantics), palette entry enabled in assembly context. Esc lifecycle is
   already the pipeline's.

Out of scope (unchanged backlog): farthest-distance and explicit
parallel-face callouts; measure between a part tab and an assembly tab.

### Tests

- Core: two unit-cube "occurrences", one translated (10,0,0) — transformed
  `DistanceDetailed` between facing named faces = 9, closest points on the
  correct sides. A rotated case (90° quat) to catch transform-order mistakes.
- GUI: assembly with two occurrences, arm measure, click a face on each,
  assert readout contains the distance and `measure.picked` holds two
  elements with different occurrenceIds.

---

## Order & effort

B (assembly measure) first — it is plumbing along proven seams (~1 session),
and A's visual work benefits from uninterrupted screenshot iteration on
dev-watch. Neither changes the file format; no golden bumps.
