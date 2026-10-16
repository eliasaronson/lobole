# Standard parts

Bought hardware — screws, nuts, washers, bearings, retaining rings —
generated from vendored ISO and DIN dimension tables and placed in assemblies, so the BOM lists what to buy, mass
properties include the hardware, and interference and contact checks see it.
A fastener's size and length are **derived** from the hole it is picked in,
never typed: a wrong-length screw looks right in the viewport.

Engineering catalogs (user-imported bearing data) are on their own page:
[catalogs.md](catalogs.md).

## The data

`assets/parts/`, staged by CMake to `build/assets/parts` and installed to
`share/lobole/parts`; `RuntimePaths::PartsDir()` (`src/common/RuntimePaths.h`)
is the one place the directory name is written, under `DataDir()`
(`LOBOLE_DATA_DIR` overrides it).

| file | from | licence |
|---|---|---|
| `iso4762def.csv`, `iso14579def.csv`, `iso4017head.csv`, `iso10642def.csv`, `iso7380def.csv`, `iso4032def.csv`, `iso7089def.csv`, `iso888length.csv`, `iso10664def.csv` (torx lobes), `iso4757def.csv` (Phillips, unused) | FreeCAD Fasteners Workbench | GPL-2.0-or-later |
| `clearance_hole_sizes.csv`, `tap_hole_sizes.csv` (metric rows only) | bd_warehouse | Apache-2.0 |
| `asmeb18_3def.csv` | typed from ASME B18.3-2012 Table 1 ([inch screws](#inch-screws-asme-b183)) | the standard's numbers |

Each file keeps its upstream name and content below a provenance header, so a
re-vendor can diff; attribution is in `THIRD_PARTY.md`.

`src/core/parts/FastenerTable.{h,cpp}` reads them. A file can hold several
tables separated by blank lines (`iso4762def.csv` holds the dimensions, the
nominal length series, and the length range each size is made in), with or
without a table-name line. A blank cell is absent from the row, not zero; a
non-numeric cell (a torx driver size, `T30`) is text and absent from the
numbers. A parenthesised size (`(M3.5)`) is a size. A malformed file is
refused naming the line, and a missing data directory is refused naming the
path it looked in.

**A vendored table is a fixed scope.** An unstocked size is refused naming the
nearest stocked sizes and then the full list — never interpolated or rounded,
because a screw at a size nobody makes is plausible and cannot be bought.
Lookups: `Dimensions(stem, size)`, `NominalLengths`, `LengthRange`,
`SnapLength` (shortest stocked length ≥ required, within the size's range) and
`SnapLengthDown`, `ThreadForClearance` (diameter → thread and fit, refused when
it matches nothing *or several* — 46 of the table's 111 diameters are a
clearance for two threads), `WidestClearance`, `HoleDiameter` and
`CoarsePitch` (used by the Hole feature's fit,
[../modeling/holes-and-threads.md](../modeling/holes-and-threads.md#hole)).

## The model: a fastener is a part, placed as an occurrence

`FastenerFeature` (`src/core/features/FastenerFeature.{h,cpp}`, JSON type
`Fastener`): `standard`, `size`, `length` (+ `length_expr`), `drive`. Every
other dimension is read from the table row, which is the specification — so a
test checking the generated body against the row checks it against the
standard.

`src/core/parts/FastenerProfile.{h,cpp}` says which standard is which shape:

| standard | what | length is measured |
|---|---|---|
| `iso4762` | socket head cap screw, hex drive | under the head |
| `iso14579` | hexalobular (torx) socket head cap screw | under the head |
| `iso4017` | hex head screw | under the head |
| `iso7380` | button head socket screw | under the head |
| `iso10642` | countersunk socket head screw | overall |
| `iso4032` | hex nut | — |
| `iso7089` | plain washer | — |
| `asme_b18_3` | socket head cap screw, inch (UNC) | under the head |

- **The standard fixes the drive.** An empty `drive` is the standard's own; a
  different one is refused naming the standard that has it (a torx socket cap
  screw is an ISO 14579, not an ISO 4762 with another recess). Drives are
  `hex`, `torx`, `none`.
- Every fastener is built with its **bearing face on z = 0**, body below: a
  screw's head at −z and shank at +z, a nut and a washer at −z. That one
  convention lets placement point any of them along a hole axis without
  knowing which it holds.
- Material defaults to `steel` (a slug; the feature stamps its own material).
  The thread is cosmetic, so a screw is a full-diameter shank and a nut a
  full-diameter bore: generated screws are slightly heavy and nuts slightly
  light. A screw's shank carries an external thread region, so it engages a
  tapped hole or an engagement-enabled thread instead of clashing.
- A button head's socket opens through the crown, so its head is shorter than
  the table's `k`.

**One generated part document per distinct `(standard, size, length, drive)`**,
named as its designation (`ISO 4762 M6x20`), reused by every occurrence of that
spec, and filed in the **"Standard parts"** tab folder (`kStandardPartsFolder`)
so that twenty of them do not bury the person's own parts in the tab bar.
`bom` counts eight identical screws as one row of quantity 8 without knowing
about fasteners. A fastener is **refused on a part tab**: CAM would machine a
screw body in a part, mass and BOM would count it as the part, and
`check_geometry` would flag it as a disconnected component.

## Inch screws (ASME B18.3)

`asme_b18_3` is the inch hexagon socket head cap screw, #4-40 to 3/4-10
UNC, 1/4-20 and 3/8-16 among them (the camera and tripod threads). No
permissively licensed inch table carries B18.3's heads, so
`assets/parts/asmeb18_3def.csv` is typed from ASME B18.3-2012 Table 1 and
says so in its header, column by column: body diameter D, head diameter A
(`dk_max`), head height H (`k`), socket size J (`s_mean`) and key
engagement T (`t`), max or min as the table gives them. The nominal lengths
are the standard's recommended series (1/8 in. steps to 1 in., 1/4 in. to
3-1/2, 1/2 in. to 7, 1 in. to 10); the per-size length range is not the
standard's but a conservative envelope of what suppliers stock, so a length
outside it is refused rather than drawn.

The file is in the inches the standard prints, and a `# units: inch` line
in its header scales every value -- and the length series' keys -- to mm
once, on load (`TableFile::mmPerUnit`): nothing past the loader sees an
inch. A size is keyed as a supplier writes it (`1/4-20`), its thread is the
UNC one the thread table names (`1/4-20 UNC`, so a tapped 1/4-20 hole
engages it), and the designation writes the length in inches, as a
fraction where it is one: `ASME B18.3 1/4-20 x 3/4`. An unknown size is
refused naming the eleven there are.

`add_fastener {standard: "asme_b18_3", size: "1/4-20", length}` places one
(`length` in mm: 19.05 for 3/4 in.). `fit_fastener` and the Fastener tool
do not offer it: a fit derives the joint from the clearance, washer and nut
tables, which are metric, and is refused for an inch screw naming
`add_fastener`.

## Deriving a fastener from a hole

`ui::FitFastenerToHole` (`src/ui/FastenerActions.cpp`, declared in
`src/ui/AssemblyActions.h`), RPC `fit_fastener`:

- **The pick is a circular edge — the rim the head sits against.** A
  cylindrical face has an axis but no direction. An element name is shared by
  every placement of a part and by every part built the same way, so the pick
  carries the occurrence as well; when several placements answer to the name
  and none is given, it is refused listing them.
- **A rim may be arcs.** The faces around a hole split its rim wherever they
  meet it — a seat that was two faces before the hole was cut, a slot across
  it — so the picked edge may be an arc. `Measure::RimCircleOf` gathers every
  edge of the same body on that circle (centre, axis and radius to a
  tolerance relative to the radius) and the fit is to the whole circle. Arcs
  that do not close it are refused: "this rim is N arcs covering M°".
- **Size**: the Hole behind the rim (`drawing::FindHoleForRef`) says its
  thread if it recorded a fit; otherwise the diameter is read backwards out of
  the clearance table, and an ambiguous diameter is refused naming the
  candidates (`size` then says which, and is checked against the hole).
- **The stack** (`parts::TraverseAxis`, `src/core/parts/AxisStack.{h,cpp}`):
  a ray down the hole's axis hits nothing — the hole is a void — so the probe
  is a bundle of four rays parallel to the axis just outside the bore wall,
  which measures each plate's thickness. A body the axis crosses twice (a
  folded part) is two segments. Between two crossings a ray is classified at
  the ends of the interval as well as its middle, and an end that is not
  material is walked in to where it starts: the intersector can miss a
  crossing (a drill point's apex, grazed 1e-13 off the axis), and a blind
  tapped hole then read as solid from its mouth. A second, on-axis ray finds
  what actually stops a screw, and comparing the two classifies the joint:

  | on-axis ray | means |
  |---|---|
  | clear | a through joint |
  | blocked at the rim itself | the edge is the end of a solid (a boss, a pin), not a hole — refused |
  | blocked inside the last part, past its face | a blind hole |
  | blocked at a part's face | that part has no hole on this axis — refused |
  | blocked in an earlier part | its hole does not go through — refused |

  A gap between plates is spanned and named in the summary, never absorbed:
  `M6x45 — grip 41: Top 12, gap 3, Bottom 26`. A gap is either an unmodelled
  gasket or a defect, and saying so is what makes the second visible.
- **Where the joint ends** is `parts::JointEnd`, shared with
  `check_fasteners` so the two agree: a blind floor, the end of a tapped
  thread the screw engages (`ThreadEndAlong`), or a caller's bound — the
  optional second pick, or (when checking) the joint's own nut, the first nut
  along the axis and any jam nut against it. A gap longer than the longest
  screw of that size is made in also ends it, at its near side: past it is
  another part that happens to lie on the line (the pan/tilt head's block
  screws point at the camera 200 mm above the yoke), and without the bound
  the joint was sized to reach it and the nut refused as a blind hole.
- **Length.** Through: grip + washers + the nut's height (every thread of the
  nut engaged), snapped **up** to a stocked length. Blind: snapped **down** so
  the screw cannot bottom out, and refused if the hole is shallower than one
  nominal diameter of engagement. A blind **clearance** hole is refused — a
  machine screw has nothing to bite — naming the two fixes (tap it, or a nut on
  the far side); the hole asked is the one the axis stops in.
- **The joint**: `{nut, washer_under_head, washer_under_nut}` say what it is
  made of — geometry cannot imply them — and the length accounts for them. A
  nut on a blind hole is refused.
- **Countersunk** (`iso10642`): the hole must be a countersink Hole, the pick
  must be the countersink's rim (not the bore's), the countersink must be deep
  enough for the head, and no washer goes under it.
- **Second pick** (`far_end`): where the joint ends when the axis carries on
  into material the fastener does not clamp. It truncates; everything between
  the two picks is still traversed. A pick on the head's side of the rim or off
  the axis is refused.
- `dry_run` derives without placing. `FastenerFit::plan` lists every piece
  (screw, washers, nut) with its position and orientation on both paths; the
  commit walks it.

### Attached, and mated

- Each placed piece **rides on** an occurrence (`Occurrence::attachedTo` with
  `attachOffset`/`attachRotation`, `src/core/Occurrence.h`, format v72): the
  screw and its washer on the part the head bears on, the nut and its washer
  on the last part in the stack, so a joint that opens does not stretch a
  rigid group across it. `assembly::ApplyAttachments`
  (`src/core/AssemblySolve.{h,cpp}`) recomputes riders' `position` and
  `orientation` from their parent after every solve — composition, not a
  constraint; no solver sees it; one level only. An attached occurrence gets no
  drag gizmo (its pose is derived, so a drag would be overwritten), moves with
  its parent while that is dragged, and is deleted with its parent — a screw
  whose plate is gone would be floating hardware again. **Detach from its
  part** in the Occurrences context menu releases it. The relative placement
  is stored explicitly rather than reinterpreting `position`, so every reader
  of `position` keeps one meaning; it is quaternion arithmetic, exact for a
  rigid placement.
- **It follows its HOLE, not just its part** (format v157). A hole can move
  inside a part that does not move — a plate made thicker raises its top
  face, a bore pitch changes — and an offset fixed in the part's frame left
  the hardware where the hole used to be: on the pan/tilt head, `base_t`
  2 mm more stood twelve pieces 2 mm into the yoke. So each piece is
  **seated** on two named faces of its parent, `Occurrence::attachSeat`:
  `face`, the planar face it bears on (the head's face, or the far face for
  a nut and its washer), and `axis`, the cylinder or cone whose axis centres
  it (the bore, or a countersink's cone), with an `innerPath` into a
  sub-assembly. `assembly::SeatFrame` derives the seat's frame from them on
  every solve — origin where the axis meets the face's plane, Z the face's
  outward normal, X the parent's X projected into the plane — each face
  resolved through `ConnectorLocalFrame`, so the piece behaves as a
  concentric + coincident mate to those faces would. With a seat,
  `attachOffset`/`attachRotation` are in its frame (a washer's thickness
  under a head, the turn of a nut).
- **It follows its faces through a later modification, not a split.** A
  chamfer on the hole's rim modifies the bore and the face round it, so
  their names gain `;M<chamfer>`
  ([naming](../modeling/toponaming.md#naming-grammar)); a seat name that no
  longer resolves is followed to the ONE face whose name is it plus whole,
  unsplit `;M<tag>` steps (`ConnectorLocalFrame`'s `followModified`, which
  a mate does not ask for). The stored names are not rewritten, so undoing
  the chamfer finds them as they were. A face cut in pieces (`;M<tag>[…]`,
  [split pieces](../modeling/toponaming.md#split-pieces))
  names no one face to sit on, and the seat is lost saying which feature
  split it: on the pan/tilt head a joint-screw hole breaking out through the
  part's end split its bore in two (`feature 39 split the face into 2
  pieces`), which is the model to fix, not the seat.
- **Any standard part, not just a screw.** Nothing in a seat is a
  fastener's: a retaining ring against its groove's flank, a part against a
  shaft's shoulder, are the same two faces. `assembly::SeatRider` seats an
  attached occurrence on faces the caller names, keeping it exactly where it
  is; finding a fastener's hole is the one fastener-specific step.
- **Not mates, on purpose.** A fastener is a zero-DOF leaf: as mates, every
  piece would add rows to the mechanism the solver and the Mates list read
  (four per joint, four hundred for a hundred-bolt frame), and frame
  alignment could move hardware off the pose the fit computed. The
  attachment stays a composition after the solve. A seat's frame is cached
  on the rider against its part's regeneration (`InstanceId`, `Version`),
  and a face many riders share is resolved once per solve: 144 pieces in
  36 holes of one plate re-seat in about 6 ms after the plate regenerates
  and 0.01 ms when it has not.
- **Finding the hole** (`assembly::FindHoleSeat`): at the pose the fit
  computed, the nearest planar face of the parent square to the piece's
  axis, facing it, with a circular edge centred on that axis; and a coaxial
  cylinder running into the material from that plane (a counterbore's floor
  has two), else any coaxial cylinder in it, else a cone. `ui::AddFastener`
  seats every attached piece this way (`assembly::BindRiderToHole`); one
  placed off any hole rides its part's frame, says why in `attach_warning`,
  and `fit_fastener` lists it in `warnings`.
- **A seat that no longer resolves** — its face deleted, or renamed by a
  later feature — is an error ON THAT PIECE, never a silent stay-put: it is
  left where it last stood, its `attach_error` says which face and why (with
  the near-miss names a mate would offer), the solve counts it
  (`errored_attachments` and `attachment_errors` in `solve_assembly`), the
  Occurrences row reads "hole lost", and `check_fasteners` lists it.
- **Files from before v157** carry offsets in the part's frame and load
  unchanged. The first solve of the assembly in a session
  (`assembly::BindRidersToHoles`, from `ui::SolveActiveAssemblyReport`)
  seats each such piece in the hole under it at the pose its offset gives;
  one with no hole there keeps its offset and its `attach_warning`.
- Placing also adds a **fastened mate named `Bolted`** across every interface
  the joint spans (consecutive parts along the axis, hardware excluded), for
  every bolt, anchored on the two touching faces and snapped to each face's
  rim of the hole (`snapEdge`: the connector's origin is the hole's centre),
  so the joint holds the HOLES in line. Anchored on the faces alone it held
  their centroids, which move against the holes when a part changes shape:
  the pan/tilt head's worm blocks missed their joints by 5 mm at a heavier
  load. A snap edge that no longer resolves (a chamfer took the rim) errors
  the mate rather than falling back to the face's centroid. Its degrees of
  freedom are measured from the parts' current poses through the solver's
  own `ConnectorLocalFrame`, so the solve that follows moves nothing. Several bolts
  across one interface close consistent loops, which the loop solver reports as
  `redundant_mates` rather than errors. The ids come back as `mates`.

## Fitting a fastener: what the axis crosses

`fit_fastener` (`ui::FitFastenerToHole`) spends almost all its time deriving
the stack. Two prunes keep that proportional to what the bolt goes through:

- **Rim search by id.** An `ElementRef` names a feature, so leaves are
  filtered on `createdBy == rim.element.featureId` before `Measure::Describe`.
- **Axis traversal** (`parts::TraverseAxis`, `src/core/parts/AxisStack.cpp`)
  slab-tests the four-ray bundle, in each body's local frame, against the
  body's memoized bounds and skips bodies no ray can reach (the viewport hover
  pick's prune), and builds one `BRepClass3d_SolidClassifier` per body.

What remains is exact OCC ray-shape intersection: a mesh first pass would
trade exactness in the one number a fastener must get right, its length.
`parts::ThreadEndAlong` finds where tapping ends. `tests/test_parts.cpp`
("Axis traversal ignores the bodies its ray cannot reach") pins it.

## Checking placed fasteners

`ui::CheckFasteners`, RPC `check_fasteners`: every placed fastener re-derived
against the stack it is in now — too short, longer than its stack needs (a
through screw a shorter stocked length would span: its plate got thinner and
the nut followed the face in), bottoming out, spanning nothing, or no longer
in its hole ([attached, and mated](#attached-and-mated)). A hole that moves
takes the head or the nut with it, so a joint whose length changed is a
finding here rather than hardware buried in a plate.
Nothing is stored at placement: a fastener's position and orientation are its
rim and axis, so the check re-traverses from scratch (probing past the
**loosest** clearance for its size, since a placed fastener does not know its
hole), and it checks fasteners nobody derived. This is how a length that
depends on other documents' geometry is kept honest: regeneration cannot bind
it, so it is reported. Returns `examined`, `skipped` (a standard this build
cannot size is not judged), `ok` and `findings`.

## BOM and drawings

A BOM row whose part's whole feature list is one Fastener carries `standard`
and `designation` (derived, so it survives a tab rename); both go in the CSV
export and the `bom` RPC, and are empty for modelled parts. The
`fastener_callout` drawing dimension (assembly views only) is a leader naming
the occurrence's designation — the same string the BOM carries.

## GUI

- **Fastener tool** (`###tb_fastener` on the assembly toolbar, command
  `assembly.fastener`; `src/ui/FastenerTool.{h,cpp}`): a tool session, not a
  form. It opens with the rim pick armed; the window offers the screw
  standards whose head stands proud as buttons labelled with what each is
  (countersunk `iso10642` is reached through `fit_fastener`), washer and nut
  toggles, and
  once a rim is picked the optional far-end pick with Clear. The derivation
  summary (or the refusal) is shown live above Place, and Place stays disabled
  until there is a fit. After Place the pick re-arms with the same options —
  holes come in patterns — and the window tallies placed and joined; Done
  ends the session. With nothing picked, Place is refused visibly.
- **Ghost preview**: `SyncFastenerGhost`, called every frame, draws the
  planned pieces as translucent blue instances through the mesh renderer's
  ghost pass, rebuilt only when the plan or options change and removed when the
  session ends. It follows tool state, which no document version tracks, so it
  cannot live in the version-gated renderer resync. The offline ray tracer
  (`render_still`) does not draw it; `screenshot` does.
- The tool's pick slot is registered in the tool-session table
  (`kToolSessions`, `src/ui/FeatureEditor.cpp`) that both routes a pick and
  keeps the slot alive; `tests/test_conventions.cpp` fails if a session
  sentinel is missing from it.
- **Check fasteners** (`###bom_check_fasteners`) sits beside the Bill of
  materials in the Occurrence panel, with its findings listed under it.

## Bearings

Deep-groove ball bearings, generated from their designation the way a
fastener is from its standard and size: one part per designation, named
`ISO 15 6200`, filed in "Standard parts", placed as an occurrence, counted by
`bom` as a bought row (`standard` `iso15`, `designation` `6200`).
`list_bearings` answers the table itself, smallest bore first, so a part can
be sized to a bearing before one is placed: the pan/tilt head takes the first
60-series row over the shaft its load needs (`parts::BearingsFrom`).

**The table** (`src/core/parts/BearingTable.{h,cpp}`) is ISO 15's boundary
dimensions — d, D, B and the smallest chamfer r_s min — for series 618, 619,
60, 62 and 63 at bores 3 to 30 mm, typed from the standard's tables
and cross-checked against bd_warehouse's and BOSL2's bearing data. Like the
fastener tables it is a fixed scope: an unknown designation is refused
naming every one carried, and a shield or seal suffix (`6200-2Z`, `608ZZ`)
is refused naming the bare designation, since the BOM would otherwise call a
sealed bearing open. It also carries ISO 492's normal-class tolerances on
the mean bore and outside diameter (0/−8 µm on a 6200's bore), which is what
`check_fits` reads for a ring. Abutment dimensions are a supplier's, not the
standard's, and are carried from one cited catalogue
([abutment limits](#abutment-limits)); load ratings and speeds come from an
imported catalog ([catalogs.md](catalogs.md)).

**The body** (`BearingFeature`, `src/core/features/BearingFeature.{h,cpp}`,
JSON type `Bearing`, format v141) is one solid of revolution drawn as a
shielded bearing: an inner and an outer ring standing proud of a recessed
shield on each side, the four outer corners chamfered at r_s min, and the
ball space an internal toroidal void. One solid rather than rings and balls
because balls are a boolean each and hidden behind a shield anyway, two rings
would be two disconnected solids, and the void keeps the mass honest (a steel
608 weighs 12.2 g against a catalogue's 12 g, a 6200 31.0 g against 32 g).
The proportions inside the envelope (lands 0.3 of the section, shields
recessed 0.05 B, a ball space of 0.6 of the smaller of the section and B) are
drawing choices, named once in the .cpp; none is an ISO 15 dimension. It
cannot turn: a mechanism mates it rigidly to one seat and turns on the other.
Its axis is local +Z with side A on z = 0.

Its faces are named by what they are: `<id>/bore`, `/outside`,
`/inner_side_a|b` and `/outer_side_a|b` (a shaft shoulder bears on the inner
ring, a housing's on the outer), `/shield_a|b`.

### Abutment limits

How far a shoulder must and may reach is not in ISO 15; each supplier
publishes it. `parts::AbutmentOf` (`BearingTable`) carries, for every
designation of the table SKF lists, the "Abutment and fillet dimensions" of
SKF's *Rolling bearings* catalogue, PUB BU/P1 17000 EN (2018), product table
1.1 "Single row deep groove ball bearings", pp. 260-273:

| limit | judges | too far means |
|---|---|---|
| d_a min | the shaft shoulder's diameter, from below | the inner ring's side face is unsupported |
| d_a max | the shaft shoulder's diameter, from above | it reaches the shield, and leaves no ring face for a puller |
| D_a max | the housing shoulder's bore, from above | the outer ring's side face is unsupported |
| r_a max | either fillet | the ring rides up it instead of seating |

- SKF prints d_a max only for a CAPPED bearing; the open row's column is a
  dash. The body is drawn shielded, so every limit of a row is read from
  the 2Z row, or from the capped row SKF lists where it lists no 2Z (2RS1
  for 61804-61806 and 61904-61906), or from the open row with no d_a max
  where it lists no capped one (618/4-618/9). The row names its `variant`.
- A value SKF does not publish is absent, not invented. SKF lists no 618/3,
  619/3, 605 or 606, so those have no row, and SKF gives no D_a min for a
  deep-groove ball bearing.
- bd_warehouse's SKF-derived table (bores to 10 mm) agrees with every row
  it has; `tests/test_bearings.cpp` checks rows against the catalogue.

`list_bearings` returns each row's limits (`d_a_min_mm`, `d_a_max_mm`,
`D_a_max_mm`, `r_a_max_mm`, null where absent) so a generator models its
shoulders inside them, and `check_seat` judges against them
([fits and seats](#fits-and-seats)).

### Placing one on a seat

`ui::FitBearing` (`src/ui/BearingActions.{h,cpp}`), RPC `fit_bearing`. The
pick is the **seat**: a cylindrical face, or the circular rim where it meets
its shoulder. `parts::SeatOf` (`src/core/parts/BearingSeat.{h,cpp}`) reads it
off the part: shaft or housing bore (from the face's outward normal), its
diameter, and at each end whether a square face stops a bearing there, how
far it reaches, and any fillet into it — the bearing sits on the shoulder
plane past the fillet. A face with a shoulder at both ends or neither is
refused asking for the rim.

- **Which bearing.** A shaft takes the rows whose d is its diameter, a
  housing bore those whose D is. A Ø10 journal takes five (61800, 61900,
  6000, 6200, 6300), so `designation` or `series` says which, and several
  with neither is refused naming them; a designation of another size is
  refused naming the ones that fit. A seat that is not a stocked size is
  refused: a fit is a tolerance class on the seat, not a different diameter.
  A seat on a PRINTED part (`set_print_process`) is read at the diameter it
  prints, so a PETG housing modelled Ø32.3 for a 6002 takes the 6002 and the
  summary says "modelled Ø32.3, prints Ø32"; `check_seat` judges that printed
  diameter and reports the modelled one beside it.
- **The mate.** A fastened mate `Bearing seat` between the ring (bore to a
  journal, outside to a housing bore) and the seat, its degrees of freedom
  measured from the poses so the solve moves nothing — a fastener's `Bolted`
  mate, done the same way. The other ring is read, and mated only when
  asked: see [both rings](#both-rings).
- `dry_run` derives without placing; `candidates` lists what fits either way.

### Fits and seats

- **`check_fits`** reads a bearing ring as one side of a round mate with the
  ring's own ISO 492 band, and the ring says which side it is (the bore is
  the hole). The other side takes a class or explicit deviations; a class of
  the wrong case is refused. k5 on a journal is transition in an H7 hole and
  interference on a bearing bore, which is why the band matters. A bearing
  fit is **reported**, never judged: which ring wants the tight fit depends on
  which one turns relative to the load, and the model does not say
  ([../tolerances/README.md](../tolerances/README.md#checks)). A seat on a
  PRINTED part is read at what it prints, so a PETG housing's bore for a
  Ø22 outside is modelled Ø22.3 and the fit says so
  ([../tolerances/README.md](../tolerances/README.md#printed-parts)).
- **`check_seat {occurrence}`** (`ui::CheckSeat`) checks a placed bearing
  against the seats its rings are mated to, read with the same `SeatOf`: the
  seat is the ring's basic size, the ring sits on its shoulder (within
  0.01 mm), and the shoulder and its fillet are within the supplier's
  [abutment limits](#abutment-limits): a shaft shoulder below d_a min or
  above d_a max, a housing shoulder's bore above D_a max, a fillet above
  r_a max each FAIL, the check naming its `limit`, `source` and `variant`.
  A limit the catalogue does not print is unverified with the reason — d_a
  max for 618/4-618/9; every limit for 618/3, 619/3, 605 and 606, whose
  shoulder is then judged only to reach past the ring's chamfer and whose
  fillet against r_s min. The row used is returned as `abutment` (null for
  those four). An installed catalog (`catalog`, `revision`, `hash`,
  `designation?`) is checked against the same measurements on top. Both
  rings are checked, the one no mate reaches through the seat it sits on
  ([both rings](#both-rings)); a ring with no seat found is unverified,
  never a pass. A ring with no shoulder at the end it sits at is located
  by a retaining ring seated in a groove beside it
  ([a ring in its groove](#a-ring-in-its-groove)) — `<ring>_ring_located`
  passes and `<ring>_retainer_reach` judges the ring's installed reach by
  the same abutment limits — and with neither it is
  `<ring>_ring_located: unverified`.

### Both rings

The body is one solid, so in a mechanism it is mated to ONE seat: fastened
to the housing and to the shaft at once, it would lock the shaft to the
housing. The other ring still sits on a seat, and that seat is what is
checked:

- **`check_seat`** reads a ring no mate reaches through the cylinder under
  it: a face of another placement on the bearing's axis (within 1 µm, the
  directions within 1e-5), of the ring's sense (a shaft for the bore, a
  bore for the outside), running under the ring's width, and within 5 % or
  0.5 mm of its size, so an unrelated coaxial face (a clearance bore, a hub)
  is not taken for a seat. The nearest diameter wins, and the usual checks
  then judge it: a bore of another size FAILS the diameter rather than being
  passed over. Its entry in `seats` says `found: coaxial` with `mate: null`,
  and names the placement and face (`occurrence`, `inner_path`,
  `feature_id`, `face`).
- **`fit_bearing`** places and fastens the picked ring as before, then reads
  the other ring the same way and returns it as `other_ring` {ring, seat?,
  diameter_check?}. It does NOT mate it unless asked: in a mechanism the
  shaft already turns in the housing by a joint of its own, and a second
  joint through the bearing closes a redundant loop for the solver to
  reconcile. `mate_other: true` adds a **revolute** mate (`Bearing inner
  ring` / `Bearing outer ring`) from that seat to the ring, as placed, for an
  assembly where the bearing IS the joint. Asked for with no seat under the
  ring, it says so in its own answer (`other_mate` 0, the summary) and
  leaves the placement it was asked for first.
- **`check_fits`** reads fits off mates, so the other ring's fit is listed
  once `mate_other` (or a hand-made mate) names its seat.

### GUI

**Bearing tool** (`###tb_bearing`, command `assembly.bearing`,
`src/ui/BearingTool.{h,cpp}`): a tool session like the fastener's. It opens
with the seat pick armed (a face or an edge); once a seat is picked the
bearings its diameter takes are offered as buttons (`###bearing_des_6200`),
the summary or refusal is shown live, a translucent ghost stands where Place
would put it, and Place re-arms for the next seat.

### Tests

`tests/test_bearings.cpp` (the table against ISO 15, the abutment rows
against SKF's and the absent ones absent, the refusals, ISO 492
bands, the body's named faces and its volume by Pappus, round trip, placing
on a journal by face and by rim, a housing bore, a part-tab refusal, the BOM,
`check_fits` on a k5 journal, `check_seat` with a square shoulder inside
d_a, one below d_a min, one above d_a max, a housing shoulder past D_a max,
a fillet past r_a max, a 606 SKF does not list, and a catalog, both rings — the housing found under an unmated
outer ring, a wrong-size bore failing, `mate_other`'s revolute moving
nothing, and the answer when nothing is there), GUI `parts/a_bearing_is_placed_on_a_picked_shaft`.
Golden `v141-bearing`.

## Retaining rings

DIN 471 rings for shafts and DIN 472 rings for bores, and the groove each
sits in — what locates a bearing ring on the side its shoulder does not.

**The table** (`src/core/parts/RetainingRingTable.{h,cpp}`) is typed from
Seeger-Orbis's data charts, "Seeger-Rings for shafts A3 - A56 / DIN 471"
and "Seeger-Rings for bores J8 - J58" and "J60 - J150 / DIN 472" (the
Seeger catalogue's sections 10 and 11, the datasheet PDFs on
seeger-orbis.com): DIN 471 sizes 3 to 56, DIN 472 sizes 8 to 72 (the
largest outside diameter the bearing table carries, a 6306's, is 72). Per
size, the ring — thickness s, free diameter d3 (a 471 ring's inside, a 472
ring's outside), lug height a, section b, lug hole d5 — and the groove —
diameter d2, width m (min), depth t, edge distance n (min). A fixed scope:
a size it does not carry is refused naming the nearest and the full list
("no DIN 471 ring for a Ø43 shaft — the nearest carried are 42 or 44"),
and a standard other than `din471` or `din472` is refused naming both.

**The ring** (`RetainingRingFeature`,
`src/core/features/RetainingRingFeature.{h,cpp}`, JSON type
`RetainingRing` {standard, size}, format v156) is SIMPLIFIED: a flat annulus
s thick at the FREE diameter with the table's section b — no opening and no
lugs, whose angular extent the table does not give. A 471 ring's inside is
d3, its outside d3 + 2b; a 472's outside d3, its inside d3 - 2b. Free, a 471
ring is smaller than its groove (d3 < d2; that grip is what holds it), so in
its groove the drawn ring overlaps the floor by (d2 - d3)/2. Axis +Z, side A
on z = 0; faces `<id>/bore`, `/outside`, `/ring_side_a|b`. Steel.

`add_retaining_ring {standard, size, position?, orientation?}`
(`ui::AddRetainingRing`, `src/ui/RetainingRingActions.{h,cpp}`) places one
where told, as `add_bearing` does: one generated part per spec, named
`DIN 471 25x1.2`, in "Standard parts", refused on a part tab and refused
before a tab is made for an unknown size. `bom` counts it as bought,
`standard` `din471`, `designation` `25x1.2`. `list_retaining_rings
{standard, min_size_mm?}` answers the table, groove included.

### The groove

`RetainingRingGrooveFeature` (`src/core/features/RetainingRingGrooveFeature.{h,cpp}`,
JSON type `RetainingRingGroove`, v156) cuts the groove in a part:

- `face`: a cylindrical face — a shaft takes `din471`, a bore's wall
  `din472`, read from the face's normal like a Thread's. `standard?` may
  name it; the other one is refused.
- `from`: a circular edge at ONE END of that face, the side of what the
  ring holds — the rim where a journal meets the shoulder its bearing sits
  on. `offset` (+`offset_expr`) is the distance from it into the face to
  the groove's NEAR flank, the one the ring bears on: the bearing's width,
  for a bearing held between the shoulder and the ring.
- On a PRINTED part (`set_print_process`) the face is read at the size it
  prints, as `fit_bearing` reads a seat — a PETG journal for a Ø25 ring is
  modelled Ø24.9 and takes the Ø25's groove — and the floor is cut where
  it prints at d2.
- The groove is the row for the face's diameter: d2 deep, m wide. Refused,
  never adjusted, when the face's diameter is not a carried size, the
  groove runs off the face, or less than the edge distance n is left past
  it — the land that carries the ring's thrust toward the free end; the
  refusal says the largest offset that fits.
- Faces `<id>/groove_floor`, `/groove_flank_a` (the near flank) and
  `/groove_flank_b`, roles `groove_floor` and `groove_flank`.

So a journal for a bearing located by a ring is B + m + n long past its
shoulder at least, and `list_retaining_rings` gives m and n before anything
is cut.

### A ring in its groove

`fit_retaining_ring {groove, dry_run?}` (`ui::FitRetainingRing`,
`src/ui/RetainingRingActions.{h,cpp}`) puts the ring a groove takes into
it. `groove` names any face of a `RetainingRingGroove` (its floor or a
flank) on a placed part, with the `occurrence` (and `inner_path`) it is on;
a name several placements answer to is refused naming them.

- **Which ring** is the groove's: the standard and size it was cut for
  (DIN 471 in a shaft, 472 in a bore). It is added as a bought part
  (`add_retaining_ring`'s one part per spec).
- **Where.** It is attached to the grooved placement and **seated** by
  `assembly::SeatRider` on the groove's faces by name
  ([attached, and mated](#attached-and-mated)): `groove_flank_a` is the
  face, `groove_floor` the axis, and its `ring_side_a` lies on the flank,
  the ring standing in the groove off it (the flank's outward normal is the
  seat's Z, and the ring's +Z runs from side A). So it follows the groove
  when the part changes — a journal made longer, a groove's offset — and a
  groove that is deleted leaves it in error, as a fastener's lost hole
  does. One ring to a groove: a second is refused naming the first.
- **Its grip is not a clash.** Drawn at its free diameter, a 471 ring's
  inside (d3) is under the groove's floor (d2), so the ring overlaps the
  shaft by (d2 - d3)/2 over its thickness — which is what holds it. Both
  features stamp a `RingGrip` on their body (`core/Feature.h`: the groove
  on its floor, the ring on the face that grips — a 471's bore, a 472's
  outside), as a Thread stamps the `ThreadRegion` a screw may engage, and
  `Measure::ContactBetween` relieves EXACTLY that annulus: between the ring's
  grip face and the floor, over the ring's thickness, and only for a ring
  and a groove of the same standard and size, on one axis, the ring inside
  the groove's width. Anything else either overlaps is still a clash: a
  ring half a millimetre out of its groove, or a ring of the next size in
  it. It is geometry, not a flag on the pair, so it holds wherever contact
  is asked — `interference` (a named pair reports `ring_grip_pairs` and
  `ring_grip_volume_mm3`), the exact sweep, `contact_report`, an exact motion
  study (which bypasses its contact cache for a body with grips, as for
  threads) — and needs no assembly: the seat is how a ring gets there, the
  grip is why it may be there. It is the same designed-overlap step that
  relieves a press fit
  ([designed overlaps](../inspect/README.md#designed-overlaps)): a ring has
  no tolerance to read, so its grip is stamped rather than inferred from
  bands. The mesh mode of a sweep has no grips and
  reports the overlap, as it does a thread. The geometry cache carries the
  grips (`kGeometryFormat` 5), so a reopened project relieves them too.
- **It locates a bearing.** `check_seat` reads a bearing ring that has no
  shoulder at the end it sits at as located by a ring seated (attached,
  its seat resolving) in a groove of the seat's own placement — 471 on the
  shaft for an inner ring, 472 in the bore for an outer — on the bearing's
  axis, beside the ring within the groove's float (m - s, plus 0.01 mm).
  The seat's entry names it (`retainer` {occurrence, designation, gap_mm,
  reach_mm}); `<ring>_ring_located` passes, and `<ring>_retainer_reach`
  judges the ring's reach as installed — its inside on the floor, d2 + 2b
  for a 471, d2 - 2b for a 472 — against the same
  [abutment limits](#abutment-limits) as a shoulder: d_a min to d_a max on
  a shaft, D_a max in a housing. A DIN 471 25 reaches Ø29.9, inside a
  6005-2Z's 28.2 to 31.9.

`tests/test_retaining_rings.cpp`: the table against the charts at several
sizes and against itself (t is half of d1 - d2 on every row), the refusals,
both rings' bodies and named faces, a groove cut on a Ø25 shaft from the
shoulder and from the free end (its floor, flanks and removed volume), a
groove in a Ø47 bore, every groove refusal, round trip, and placing and
counting rings in an assembly; a ring fitted into its groove (pose,
seat, a second refused, a face no groove made refused, following the groove
when its offset changes), its grip relieved in its groove and not half a
millimetre out of it or at another size, a groove on a printed shaft, and
`check_seat` reading a seated ring as locating a 6005's inner ring (and not
once its seat is lost). Golden `v156-retaining-ring`.

## RPCs

`list_bearings {series?, min_bore_mm?}` (the table's rows, smallest bore
first, each with its abutment limits: what a generator sizes a journal, its
shoulder and a housing from before anything is placed), `add_bearing
{designation, position?, orientation?}`, `fit_bearing {seat,
designation?, series?, dry_run?}`, `check_seat {occurrence, catalog?,
revision?, hash?, designation?}`; `list_retaining_rings {standard,
min_size_mm?}`, `add_retaining_ring {standard, size, position?,
orientation?}`, `fit_retaining_ring {groove, dry_run?}`;
`add_fastener {standard, size, length?, drive?, position?, orientation?}`
(places where told), `fit_fastener {rim, standard?, drive?, size?, nut?,
washer_under_head?, washer_under_nut?, washer?, far_end?, dry_run?}`,
`check_fasteners`, `bom` (`src/remote/RemoteAssembly.cpp`).

## Tests

`tests/test_parts.cpp` (the tables, their shapes and refusals, the generated
geometry against the rows — an M6x20 ISO 4762 volume computed from head +
shank − socket; ASME B18.3's 1/4-20 row in mm, its refusal, its inch length
series and a 1/4-20 x 3/4 measured against Table 1), `tests/test_actions.cpp` (`[parts]`: one part per spec and
the part-tab refusal, the four derivation cases, whole joints with washers and
nut, a second pick, a thread ends a joint, countersunk screws, a bolt mates
the parts, riders, a rim on two placements, pieces following holes that
move — a plate thickened and thinned, bores moved, a pre-v157 rider seated on
its first solve, a suppressed hole's error), `tests/fastener_stack_smoke.py`
(`fit_fastener` and `check_fasteners` agree on where a joint ends),
`tests/test_drawing.cpp` (`fastener_callout`),
`tests/drawing_frames_smoke.py` (an ASME B18.3 screw placed, refused at an
unknown size, and BOUGHT in a parts list),
`tests/test_faceselector.cpp` (a head-on rim pick takes the near rim), GUI
`parts/a_fastener_is_placed_by_picking_a_rim` (arm, refuse, pick, far end,
ghost count tracks the options, place, joined, check). Goldens
`v69-fastener`, `v70-fastener-kinds`, `assembly/v157-attach-seat`.

## Not built

- Phillips/Pozidriv drives (the Phillips table is vendored but unused);
  inch sizes beyond ASME B18.3's socket head cap screws (no inch nuts,
  washers, hex heads or UNF), and deriving an inch screw from its hole.
- Modelled helical threads on generated fasteners (their thread is
  cosmetic; a `Thread` feature on a part can model one).
- Extrusions, motors, o-rings as generated parts; a searchable parts
  browser.
- Bearings beyond deep-groove ball bearings at bores 3 to 30 mm; shield and
  seal variants as their own designations; a bearing whose rings turn.
- A retaining ring's opening and lugs; DIN 6799 (E-clips) and heavy-duty
  rings; a ring tool in the GUI (`fit_retaining_ring` is an RPC); a grip
  carried through a pattern of grooves (a Pattern copies a body's threads,
  not its ring grips).
- Folding hardware in the Occurrences tree.
- A ghost on hover before the rim is picked.
- Attachment chains (an attached occurrence's parent is never itself chased)
  and attaching anything other than fasteners through the GUI.
