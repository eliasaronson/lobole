# Bosch PES4M injection pump

The four-element inline injection pump on the OM615 (Mercedes L306D, W115
200 D), as one assembly that moves the way the real one does: the camshaft
lifts four roller tappets and plungers in firing order 1-3-4-2, the control
rack turns every plunger's helix through its control sleeve, the delivery
valves lift, the feed pump's tappet rides its eccentric, the governor
diaphragm rides the rack, the hand primer strokes, and the timing device's
flyweights advance the sprocket. It is a clearance-checked kinematic model;
no dimension is sourced (see below). It is also built into the OM615
(generators/om615/README.md#the-injection-pump), chained to that engine's
crank and timed; this generator is the pump on its own.

## Running it

```sh
tools/headless-preview.sh --start <port>
python3 generators/pes_pump/pes_pump.py --port <port> [--out scratch/pes_pump] \
    [--samples 73] [--parts NAME ...] [--no-studies] [--no-still]
```

It refuses to run against an instance with unsaved changes. `--parts` builds
only those parts; `--no-studies` stops after the build. Outputs:
`pes_pump-unchecked.dsgn` (written first, so a failed run leaves something to
inspect), `pes_pump.dsgn` (only once every check passes, left on Full load),
`pes_pump.glb` (the camshaft study), `report.json` and the renders.

## Structure

| file | what |
|---|---|
| `pes_spec.py` | the few numbers an M-size pump is quoted by, the stations derived from them, and the motion laws that BOTH the drawn cams and the assembly's relations are made from |
| `pes_parts.py` | one builder per part: housing (cam gallery, tappet and element bores, rack bore, fuel gallery, inspection window, feed-pump pad, governor flange) and window cover; the "Z" cover, the two holder clamp plates and the slotted screw that holds both; camshaft (four lobes, feed-pump eccentric, journals, drive taper); tappet, pin and roller; barrel and plunger with its vertical groove and control helix cut in; control sleeve with a full involute tooth ring; spring and seat; toothed rack; delivery valve, seat, spring and holder; feed pump body, tappet, banjo fitting, primer tube and primer; governor housing, cover, diaphragm and spring, oil filler cap, nameplate and stop lever; timing hub, flyweights, sprocket, nut |
| `pes_pump.py` | assembly, configurations, studies, checks, renders |

Frame: +X along the camshaft (drive end −X, governor +X; element 1 at the
drive end), +Z up every element's axis (camshaft centreline z = 0), +Y across
(rack on +Y, fuel gallery and feed pump on −Y). Per-element parts are drawn on
their own axis in pump heights, so a part's reported position is its travel.
Every drawn cam sits `CAM_ALLOW` (0.05 mm) inside its law.

**What an engine mounts it by** is stated in `pes_spec.py` and nowhere else,
so the engine's block is machined to the pump's numbers:

- **The flange.** The face is `FLANGE_FACE_X`, with a Ø60 register and three
  M8 clearance holes on a 78 PCD, one at the top. Behind each hole the
  housing has a pocket for its bolt's head (`FLANGE_BOLT_POCKET`, room for a
  low-head ISO 7380 M8), its floor spot-faced half a millimetre into the
  flange (`FLANGE_BOLT_SEAT_X`): without the pockets the top head stood in
  the cam case and the lower two in the lugs, and without the spot face the
  rim lay across the seam where the case meets the flange and came back in
  three arcs, which no fastener can be fitted to. The register is smaller
  than the timing device's hub (`TIMING_DEVICE_R`, 34), so an engine's bore
  has to step out ahead of it.
- **The frames.** The housing's `flange` frame sits on that face, with z
  along the camshaft toward the governor and x straight up. The housing has
  a `fuel_inlet` frame at the gallery inlet. Each delivery valve holder has
  an `outlet` frame on the top of its union nut.
- **The sprocket.** It has 44 teeth for 3/8" duplex chain, the same as the
  engine's camshaft, since both turn at half crank speed off one chain. Its
  chain plane is `CHAIN_PLANE_X` and its tips reach `SPROCKET_TIP_R`.
- **The rack.** `assemble(app, parts, rig, mount, rack_offset)` builds the
  pump into another assembly's rig and fastens `flange` to `mount`. The load
  configurations belong to the pump's own assembly, so in someone else's
  assembly the rack is held with `rack_offset` instead.
- **The part names.** The camshaft is "pump camshaft" and the governor's
  filler cap "governor filler cap", so an engine's own camshaft and oil
  filler cap keep theirs. `app.part(name)` now refuses a name another
  script created, so a clash would stop the build rather than write into
  the engine's part.

## What it looks like

The outside follows the owner's photographs: the pump on the engine, the
bare pump on a bench, and a Bosch exploded diagram. None is dimensioned, so
every size is scaled against the 25 mm element pitch and named where it is
used (`pes_spec.py`, or the top of its builder).

- **Casting.** Satin die-cast aluminium. The cam case is rounded along its
  bottom and into its ends, stands on a lug under each end with a gusset
  rising from each, and has two ribs on its +Y wall. The element block above
  it has rounded vertical edges.
- **Delivery valve holders.** Each is a plain shank above the deck, a 19 AF
  hex body, and a larger 22 AF union nut on top. The `outlet` frame is on
  the nut's top. Two flat steel clamp plates, each over a pair of holders,
  are held to the deck by a slotted screw between the pair.
- **The "Z" cover.** A large brass-olive plate on the element block's -Y
  face, with a raised Z and a slotted screw at each corner. It stops short
  of the fuel inlet boss.
- **Feed pump.** A Bosch FP/K 22M: a dark painted body turned about its
  piston's axis, with an oval nameplate boss on its outer end, a 27 AF plug
  underneath, and two banjo fittings, the inlet on top beside the primer
  and the outlet on the drive-end side. The hand primer is a steel tube
  rising from the body with a black Ø45 wheel on top: a thin knurled rim on
  six spokes.
- **Governor end.** The housing flares out of the pump body to the
  diaphragm chamber, whose cover is held round its rim by a ring of small
  screws. A red ribbed oil filler cap (the part "governor filler cap")
  stands on a boss on top, just behind
  element 4. The Bosch nameplate is on a pad on the -Y side, and the stop
  lever is on a bracket below it.
- **Colours** are linear: casting 0.35 (metallic 0.8, roughness 0.5), feed
  pump paint 0.04 and the filler cap (0.5, 0.02, 0.02), both dielectric, and
  the Z cover (0.25, 0.2, 0.08).

The +Y side faces the engine's block, so nothing on it reaches past the
housing's lugs or the governor. Nothing new reaches ahead of the flange
face.

## How it moves

Two independent drivers, as in the real pump: engine speed turns the
camshaft; the governor positions the rack.

| mate | coupling |
|---|---|
| camshaft — revolute | driver of the run study (one cam turn = two crank turns) |
| tappets — sliders | CAM relation from the camshaft, each element's lobe phased 0 / 270 / 90 / 180° for elements 1–4 |
| rollers — revolute on the tappet pin | gear relation at the base-circle rolling ratio (14/6) |
| control sleeves — revolute | RACK relation from the rack slider at the sleeve's pitch radius |
| plungers — one cylindrical mate each | CAM relation drives the slide, RACK relation the turn, so the plunger turns exactly with the sleeve whose slots its vanes ride in |
| delivery valves — sliders | CAM relation from the camshaft |
| feed-pump tappet — slider | CAM relation on the eccentric |
| control rack — slider | driver of the load study; the diaphragm is fastened to it |
| flyweights — revolutes | coupled to each other 1:1 and to the sprocket's advance |
| hand primer — slider | its own study |

Three assembly configurations hold the rack: **Stop** (0 mm), **Idle**
(4 mm) and **Full load** (15 mm). Each study NAMES the configuration it
measures, whatever is active: the camshaft turn runs under Full load, the
rack sweep under the base (configuration 0), where nothing holds the rack.
Point the camshaft study at another configuration with
`set_motion_study {configuration}`.

The pump relies on three things the API does: revolving and patterning about
a `custom` axis (the element bores), a helix's `start_deg` (the control helix
placed on its port without turning the plunger's frame), and a motion study
that names its configuration.

## What a run checks

From the study poses: each plunger's stroke equals the cam lift (7 mm); the
order the four reach top dead centre is 1-3-4-2; over the rack's 15 mm every
plunger and every sleeve turns travel ÷ pitch radius (96°); every part is
one valid solid; the exact clash sweep of all four studies is clean; and so
is an exact STANDING sweep of every pair. A motion study skips parts rigid
to each other by design, so only the standing sweep looks at what is
fastened to the housing — it found the clamp plates overlapping end to end,
their screws in the holders' shanks and the inlet banjo in the primer's
pillar, none of which the studies could see. The
rack and sleeve teeth are real involutes, so a wrong relation sign shows as
tooth clash. The effective stroke at each rack setting is computed from the
same helix law the plunger is cut to.

## Where the numbers come from

The references are eight undimensioned images: four Bosch/Mercedes exploded
views (PES M housing, element stack, delivery-valve holder, element), the
Mercedes catalogue plate with the pneumatic governor and timing device, and
one photograph. Every value is an engineering estimate consistent with a
Bosch M-size element pump, stated once in `pes_spec.py`:

| quantity | value |
|---|---|
| element spacing | 25 mm |
| plunger diameter | 6 mm |
| cam lift (plunger stroke) | 7 mm |
| camshaft speed | ½ crank |
| control-rack travel | 15 mm, stop to full load |
| control-sleeve pitch radius | 8.95 mm (module 25/(8π): the element pitch is 8 rack pitches, so every sleeve meshes at the same phase) |
| delivery-valve lift | 1.5 mm |

## Limits

- Delivery-valve timing is fixed at full load: a cam relation reads the
  camshaft only, so the valve cannot also follow the rack.
- Rollers roll at the base-circle ratio everywhere; on the lobe the true
  ratio changes with the contact radius.
- Springs are rigid, drawn at their shortest and carried by one end. The
  governor diaphragm is a rigid plate inside a fixed rubber rim.
- The governor and flyweights are kinematic only: nothing links vacuum or
  engine speed to the rack or the advance.
- The castings are rounded prisms, lugs, ribs and bosses, not the real
  pump's cored shapes. Screws are heads only: no shank goes into the part
  they hold. The feed pump sits on the camshaft's eccentric at mid-length,
  where the real one is nearer the drive end.
