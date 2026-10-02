# Bearing location: abutment limits and retaining rings

## The problem

The pan/tilt head's bearings cannot be located or checked properly
(generators/pantilt/README.md, Known):

- Two inner rings have a shoulder on one side and nothing on the other —
  the lower 6005 on the yoke's journal and the -Y 6002 on the tilt shaft.
  Designer has no part that locates a ring axially and no way to cut the
  groove one sits in, so the generator cannot model the fix.
- `check_seat` can say a shoulder reaches past the ring's chamfer and nothing
  more. How far a shoulder may reach is an abutment dimension (d_a, D_a) that
  ISO 15 does not give, so every shoulder diameter is "unverified" unless the
  user imports a catalog by hand. A shoulder too low leaves the ring's side
  face unsupported; one too high rubs the shield or seal and leaves nothing
  for a puller. Neither is caught.

## The shape of the fix

### (a) Abutment limits for every carried bearing

`parts::BearingAbutment` beside `BearingRow` in `BearingTable`: per
designation, d_a min, d_a max, D_a max and r_a max, typed from SKF's
*Rolling bearings* catalogue (PUB BU/P1 17000 EN, 2018), product table 1.1
"Single row deep groove ball bearings", pp. 260-273. The table header cites
the source and edition.

- SKF prints d_a max only for a CAPPED bearing (a shoulder above it reaches
  the shield or seal); for the open row the column is a dash. Our body is
  drawn shielded (2Z), so the row carries the d_a max of the 2Z variant, or
  of the capped variant SKF lists where there is no 2Z, and names which.
- A value SKF does not publish is absent, not invented: there are no rows
  for 618/3, 619/3, 605 and 606 (SKF lists none), 618/x carries no d_a max
  (no capped variant listed), and SKF gives no D_a min for this table.

`check_seat` judges the shoulder it already measures against them, naming
the limit and the source: on a shaft, below d_a min fails ("the ring's side
face is unsupported"), above d_a max fails ("it reaches the shield; nothing
is left for a puller"); in a housing, above D_a max fails. The fillet is
judged against r_a max where SKF gives one (else r_s min, as now). An absent
limit stays `unverified` with the reason ("SKF lists no d_a max for 61804").
An installed catalog, when passed, is still judged as before, on top.

### (b) DIN 471 / DIN 472 retaining rings and their groove

`parts::RetainingRingRow` (`src/core/parts/RetainingRingTable.{h,cpp}`):
DIN 471 (external, for shafts) A3-A56 and DIN 472 (internal, for bores)
J8-J72, typed from Seeger-Orbis's data charts "Seeger-Rings for shafts
A3-A56 / DIN 471" and "for bores J8-J58 / J60-J150 / DIN 472": ring
thickness s, free diameter d3, lug height a, section b, lug hole d5, and the
groove: diameter d2, width m (min), depth t, edge distance n (min). A fixed
scope: a size not in the table is refused naming the nearest and the full
list; an unknown standard is refused naming din471 and din472.

- **The ring** — feature `RetainingRing` (`standard`, `size`), a generated
  standard part like a bearing: a flat annulus of thickness s at the FREE
  diameter d3 with radial section b. Simplified: no opening, no lugs (their
  angular extent is not in the table). Faces named `<id>/bore`,
  `/outside`, `/ring_side_a|b`; axis +Z, side A on z = 0.
- **The groove** — feature `RetainingRingGroove`: `face` (a cylindrical face
  of the part), `from` (a circular edge at one end of that face: the side
  of what the ring holds, e.g. the shoulder a bearing sits against),
  `offset` (+`offset_expr`) mm from that edge to the groove's loaded flank,
  `standard?` (from the face: a shaft takes din471, a bore din472; the
  other one is refused). The table row is the face's diameter (refused
  when it is not a size the table carries). The groove is cut to d2, m
  wide. Refused when it does not fit the face: the groove must lie on the
  face, and past it the face must keep at least n before its far end (the
  land that carries the ring's thrust). Its faces are named
  `<id>/groove_floor` and `/groove_flank_a|b` (a nearer `from`).
- **RPCs**: `add_retaining_ring {standard, size, position?, orientation?}`
  (one generated part per spec, "DIN 471 25x1.2", in "Standard parts", BOM
  standard `din471`, designation `25x1.2`), `list_retaining_rings
  {standard, min_size_mm?}`. The groove goes through `add_feature`.

### Placement is NOT built here

Putting a ring into its groove in an assembly waits for the fastener rework
on its own branch (fasteners follow their holes' named faces,
docs/plans/fasteners-follow-holes.md there). This work does not touch
`fit_fastener`, `attachedTo`/`attachOffset` or the joint code. What it
leaves for that work: the ring's named faces (`ring_side_a`, `bore`,
`outside`) and the groove's (`groove_floor`, `groove_flank_a`) — the
pair a placement follows — and the note that the ring is drawn at its FREE
diameter, so in its groove it overlaps the floor by (d2 - d3)/2 until
placement treats that as intended. `check_seat` reading a ring in a groove
as locating the bearing's other side belongs with placement too.

## Files

- `src/core/parts/BearingTable.{h,cpp}` — abutment rows, lookup.
- `src/ui/BearingActions.cpp` — `CheckSeat` judges against them.
- `src/core/parts/RetainingRingTable.{h,cpp}` — new.
- `src/core/features/RetainingRingFeature.{h,cpp}` — the ring and the groove.
- `src/core/ElementOrigin.{h,cpp}` — roles `ring_side`, `groove_floor`,
  `groove_flank`.
- `src/core/io/SerializeFeatures.cpp`, `SerializationInternal.h` — two
  feature types, kVersion bump; `tests/golden/v<N>-retaining-ring.*`,
  `docs/versioning/format.md` row.
- `src/ui/RetainingRingActions.{h,cpp}`, `src/remote/RemoteAssembly.cpp`,
  `src/ui/Bom.cpp` — add/list RPCs, BOM row.
- `tools/designer_mcp.py`, `src/remote/RemoteInspection.cpp` (role list),
  `docs/parts/README.md`, `docs/modeling/toponaming.md`,
  `generators/pantilt/README.md` (the shoulder note).
- `tests/test_bearings.cpp`, `tests/test_retaining_rings.cpp`.

## Verification

- Table values against the cited sources at several sizes (6005, 608, 6200,
  6306; DIN 471 A10/A25, DIN 472 J22/J47), and the absent ones absent.
- Refusals: unknown standard, size, a groove on a non-cylindrical face, of
  the wrong standard for the face, one that runs off the face, one with
  less than n left.
- `check_seat`: a shaft shoulder below d_a min and one above d_a max each
  fail naming the limit; one between passes; a housing shoulder above
  D_a max fails; a bearing with no SKF row stays unverified.
- A groove cut on a Ø25 shaft: floor Ø23.9, width 1.3, at the stated
  offset, by measuring the named faces; the volume removed matches.
- The ring part's dims: thickness, bore d3, outside d3 + 2b, and the BOM
  row.
- Round trip and a golden at the new version; tools/test-fast.sh, the
  headless suite, the GUI suite and the `generators` label.
