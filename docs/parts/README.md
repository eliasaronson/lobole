# Standard parts

Bought hardware — screws, nuts, washers, bearings — generated from vendored ISO
dimension tables and placed in assemblies, so the BOM lists what to buy, mass
properties include the hardware, and interference and contact checks see it.
A fastener's size and length are **derived** from the hole it is picked in,
never typed: a wrong-length screw looks right in the viewport.

Engineering catalogs (user-imported bearing data) are on their own page:
[catalogs.md](catalogs.md).

## The data

`assets/parts/`, staged by CMake to `build/assets/parts` and installed to
`share/designer/parts`; `RuntimePaths::PartsDir()` (`src/common/RuntimePaths.h`)
is the one place the directory name is written, under `DataDir()`
(`DESIGNER_DATA_DIR` overrides it).

| file | from | licence |
|---|---|---|
| `iso4762def.csv`, `iso14579def.csv`, `iso4017head.csv`, `iso10642def.csv`, `iso7380def.csv`, `iso4032def.csv`, `iso7089def.csv`, `iso888length.csv`, `iso10664def.csv` (torx lobes), `iso4757def.csv` (Phillips, unused) | FreeCAD Fasteners Workbench | GPL-2.0-or-later |
| `clearance_hole_sizes.csv`, `tap_hole_sizes.csv` (metric rows only) | bd_warehouse | Apache-2.0 |

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
  folded part) is two segments. A second, on-axis ray finds what actually
  stops a screw, and comparing the two classifies the joint:

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
  along the axis and any jam nut against it.
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
  rigid placement. A fastener has no degrees of freedom, so a mate
  would add rows to the mechanism's list and could relocate the hardware.
- Placing also adds a **fastened mate named `Bolted`** across every interface
  the joint spans (consecutive parts along the axis, hardware excluded), for
  every bolt, anchored on the two touching faces. Its degrees of freedom are
  measured from the parts' current poses through the solver's own
  `ConnectorLocalFrame`, so the solve that follows moves nothing. Several bolts
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
against the stack it is in now — too short, bottoming out, spanning nothing.
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

**The table** (`src/core/parts/BearingTable.{h,cpp}`) is ISO 15's boundary
dimensions — d, D, B and the smallest chamfer r_s min — for series 618, 619,
60, 62 and 63 at bores 3 to 30 mm, typed from the standard's tables
and cross-checked against bd_warehouse's and BOSL2's bearing data. Like the
fastener tables it is a fixed scope: an unknown designation is refused
naming every one carried, and a shield or seal suffix (`6200-2Z`, `608ZZ`)
is refused naming the bare designation, since the BOM would otherwise call a
sealed bearing open. It also carries ISO 492's normal-class tolerances on
the mean bore and outside diameter (0/−8 µm on a 6200's bore), which is what
`check_fits` reads for a ring. Load ratings, speeds and abutment dimensions
are a supplier's, not the standard's; they come from an imported catalog
([catalogs.md](catalogs.md)).

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
- **The mate.** A fastened mate `Bearing seat` between the ring (bore to a
  journal, outside to a housing bore) and the seat, its degrees of freedom
  measured from the poses so the solve moves nothing — a fastener's `Bolted`
  mate, done the same way. The other ring is mated by hand, revolute where
  the housing turns about the shaft.
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
  0.01 mm), the fillet does not exceed r_s min, and the shoulder reaches past
  the ring's chamfer. How far a shoulder may reach is an abutment dimension
  the standard does not give, so it is unverified unless an installed catalog
  is passed (`catalog`, `revision`, `hash`, `designation?`), whose limits are
  then checked against the same measurements. An unmated ring is unverified,
  never a pass.

### GUI

**Bearing tool** (`###tb_bearing`, command `assembly.bearing`,
`src/ui/BearingTool.{h,cpp}`): a tool session like the fastener's. It opens
with the seat pick armed (a face or an edge); once a seat is picked the
bearings its diameter takes are offered as buttons (`###bearing_des_6200`),
the summary or refusal is shown live, a translucent ghost stands where Place
would put it, and Place re-arms for the next seat.

### Tests

`tests/test_bearings.cpp` (the table against ISO 15, the refusals, ISO 492
bands, the body's named faces and its volume by Pappus, round trip, placing
on a journal by face and by rim, a housing bore, a part-tab refusal, the BOM,
`check_fits` on a k5 journal, `check_seat` with a square shoulder, a fillet
too large and a catalog), GUI `parts/a_bearing_is_placed_on_a_picked_shaft`.
Golden `v141-bearing`.

## RPCs

`add_bearing {designation, position?, orientation?}`, `fit_bearing {seat,
designation?, series?, dry_run?}`, `check_seat {occurrence, catalog?,
revision?, hash?, designation?}`;
`add_fastener {standard, size, length?, drive?, position?, orientation?}`
(places where told), `fit_fastener {rim, standard?, drive?, size?, nut?,
washer_under_head?, washer_under_nut?, washer?, far_end?, dry_run?}`,
`check_fasteners`, `bom` (`src/remote/RemoteAssembly.cpp`).

## Tests

`tests/test_parts.cpp` (the tables, their shapes and refusals, the generated
geometry against the rows — an M6x20 ISO 4762 volume computed from head +
shank − socket), `tests/test_actions.cpp` (`[parts]`: one part per spec and
the part-tab refusal, the four derivation cases, whole joints with washers and
nut, a second pick, a thread ends a joint, countersunk screws, a bolt mates
the parts, riders, a rim on two placements), `tests/fastener_stack_smoke.py`
(`fit_fastener` and `check_fasteners` agree on where a joint ends),
`tests/test_drawing.cpp` (`fastener_callout`),
`tests/test_faceselector.cpp` (a head-on rim pick takes the near rim), GUI
`parts/a_fastener_is_placed_by_picking_a_rim` (arm, refuse, pick, far end,
ghost count tracks the options, place, joined, check). Goldens
`v69-fastener`, `v70-fastener-kinds`.

## Not built

- Phillips/Pozidriv drives (the Phillips table is vendored but unused), and
  any imperial sizes.
- Modelled helical threads on generated fasteners (their thread is
  cosmetic; a `Thread` feature on a part can model one).
- Extrusions, motors, o-rings as generated parts; a searchable parts
  browser.
- Bearings beyond deep-groove ball bearings at bores 3 to 30 mm; shield and
  seal variants as their own designations; a bearing whose rings turn.
- Balloons and a parts-list block on a drawing sheet.
- Folding hardware in the Occurrences tree.
- A ghost on hover before the rim is picked.
- Attachment chains (an attached occurrence's parent is never itself chased)
  and attaching anything other than fasteners through the GUI.
