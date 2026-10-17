# OM615 four-cylinder diesel

An OM615-shaped Mercedes diesel — the early 200 D/8, OM615.913: 87 × 83.6 mm,
1988 cc, five main bearings, a single overhead camshaft on a duplex chain
working the valves through rocker arms, and an inclined pre-chamber per
cylinder — built complete down to bearing shells, seals, oil galleries,
water jacket, manifolds, fasteners derived from their holes, the front
drive — belt, fan, alternator and vacuum pump — the Bosch PES4M injection
pump chained to the crank and timed, its four injection lines, and what
hangs on the outside: oil and fuel filters, coolant hoses, the heater pipe
and the dipstick, in a factory finish. It is a clearance-checked kinematic
model, not factory or manufacturing geometry. The injection pump is
`generators/pes_pump/`, built into this engine (see
[The injection pump](#the-injection-pump)).

## Running it

Against a private instance on a scratch port:

```sh
python3 generators/om615/om615.py --port PORT [--stage preview|local|full] [--out DIR] [--samples 49] [--no-export] [--profile]
python3 generators/om615/om615.py --port PORT --component fan --out /tmp/fan
python3 generators/om615/om615.py --port PORT --project /tmp/engine/om615.dsgn --update-component fan --stage preview --out /tmp/fan-update
python3 generators/om615/om615.py --port PORT --project /tmp/engine/om615.dsgn --validate-only --stage full
```

- **Stages.** `preview` (default) checks strict single-solid part validity,
  feature status and the accessory mounting structure; `local` adds the exact
  standing sweep, the alternator subassemblies' own sweeps and the mounting
  contacts; `full` adds the exact motion study over two crank revolutions
  (49 samples by default; a coarse step whose joints jump is re-solved finer
  and a jump that stays is reported, docs/assemblies/README.md#branch-jumps) and the measured strokes, lifts, firing order and the injection
  pump's turn, strokes, order and timing. `om615-unchecked.dsgn`
  is saved before validation so a failed gate leaves geometry to inspect;
  `om615.dsgn` only when accepted. `--build-only` and `--no-study` are the
  older names for `preview` and `local`.
- **Components.** `--component KEY` builds one part in isolation for shape
  iteration; `--update-component KEY --project P` rebuilds one part inside a
  saved engine through the component staging layer (`python/lobole/components.py`),
  keeping part, feature and occurrence identities and reusing unchanged
  geometry. Keys: `belt`, `fan`, `vacuum-pump` (the casting), `block`, `head`,
  `sump`, `timing-cover`, `damper`, `engine-mount-left`, `engine-mount-right`,
  `cam-cover`, `intake`, `exhaust`, `injector`, `prechamber`, `thermostat`,
  and what hangs on it: `oil-filter` (the housing), `oil-filter-can`,
  `fuel-filter`, `fuel-filter-bracket`, `upper-hose`, `lower-hose`,
  `heater-pipe`, `dipstick-tube`, `dipstick`, and the injection pump's
  `pump-bracket`. The pump's own parts are the pump generator's. A
  component comes out with its finish (below), so a part rebuilt on its own
  looks as it does in the engine. A component's fingerprint covers the
  builder modules, the finish table and the SDK; a change to `om615_spec.py`
  or the shared accessory mounting inputs refuses a component update and asks
  for a full rebuild.
- **Outputs** in `--out` (default `/tmp/om615`): the project, an animated
  `om615.glb` (unless `--no-export`), `reference-dimensions.json` (measured
  B-Rep extents with their evidence status), `validation-report.json`,
  `standing-report.json`, `motion-report.json`, `pump-motion-report.json`,
  the accessory reports and `timings.json`.

Tests: `om615_accessories_smoke` builds the real accessory B-Reps — the
front drive's and everything in `HANGING` — and checks belt tangency, strict
single-solid validity and a staged fan update; `om615_mounting_smoke` checks
the mounting fixture and that a missing pivot bolt, a severed or thin support
arm and a short block screw are each refused for their own reason.

## Structure

| file | what |
|---|---|
| `om615_spec.py` | every shared number, derived from bore, stroke, rod (145) and bore pitch (97) where the derivation is real |
| `om615_bottom.py` | block (with its casting exterior), crank, rods, pistons and rings, caps, shells, flywheel, damper, seals, sump, timing chain and cover, oil pump, water pump, pulleys, engine mounts |
| `om615_top.py` | head, camshaft, rockers, valves, guides, seats and springs, cam cover, manifolds, thermostat housing, injectors, pre-chambers, glow plugs |
| `om615_accessories.py` | belt, fan, alternator (stator and rotor subassemblies), vacuum pump and its face cam, the timing cover's pump drum, mounting brackets; what hangs on it — oil filter housing and can, fuel filter and its bracket, the coolant hoses and heater pipe, the dipstick and its tube; the injection pump's bracket, lines and line clamps |
| `om615_finish.py` | how each part looks: a table of finishes and the part each is given to |
| `om615.py` | assembly, the injection pump built in and timed, fasteners, the component table, stages and checks |

Frame: +Z up and the bore axis (crank centreline z = 0, so a piston's height
is its z); +Y along the crank, FRONT (timing chain) at −Y; +X across, which
with that front and up is the car's LEFT. Cylinder 1 is at the front.

**Which way round.** The pump side — injection pump mount, injectors and
glow plugs, both filters, the dipstick and the vacuum pump — is the car's
left, +X; the manifolds, the thermostat housing, the heater pipe and the
alternator are on the right, −X. Standing on the pump side the fan is on
your left, as in the owner's photographs. The side is one constant,
`PUMP_SIDE` in `om615_spec.py` (`MANIFOLD_SIDE` is its negative), and every
side-specific number and frame in the builders is written with one of the
two. Changing it is a re-layout, not a mirror image: fasteners stay
right-handed standard parts, frames stay right-handed, a part that turns
(the rockers, the engine mounts) is turned to its side about z rather than
mirrored, and the parts whose handedness belongs to the ROTATION rather
than to the side are the same parts either way — the camshaft's lobes, the
fan's blade pitch, the valve springs. The crank turns +Y, clockwise seen
from the front: from cylinder 1's TDC its pin moves toward +X, the pump
side (measured: +7.26 mm in x at 10°). So the timing chain's run on the pump
side is the tight one and carries the guide rail; the tensioner is on the
slack run. The oil pump, inside the chain case and the sump, is not
side-specific and does not move with it. The vacuum pump is the same unit
translated to its side, its face cam 16 mm toward +X of its centre on
either side — and that is the injection pump's axis. The injection pump
itself is not re-laid by `PUMP_SIDE`: its frame maps onto the engine's by a
fixed turn (pump +X to engine +Y, pump +Z to +Z), so its rack side faces
the block only with the pump side at +X. The belt is laid out
counter-clockwise in XZ whichever side the alternator is on
(`belt_order()`).

**The mechanism** has one driving DOF, the crank: four slider-crank loops off
one crankshaft; a chain relation at 1:2 to the camshaft and another at 22:44,
phased, to the injection pump's camshaft, which drives the pump's own
mechanism (generators/pes_pump/README.md#how-it-moves) and, 1:1, the vacuum
pump's face cam; per valve a CAM relation for its lift and a RACK relation
that makes its rocker follow; the alternator and fan coupled to the crank by
relations on pulley pitch radii, assuming no slip. The belt occupies its
running envelope and does not circulate; the timing chain is drawn crank to
camshaft and is not rerouted round the pump's sprocket. The pump's control
rack, hand primer and flyweights are the other free DOFs; nothing drives
them, and they stay at full load, rest and zero advance.

**The alternator** is two rigid child assemblies — stator and rotor — joined
by a revolute in the engine: each child is rigid in itself, and the joint
between them is the engine's. The whole-engine standing sweep checks
top-level pairs only, so the children are swept on their own, and so is the
injection pump's assembly on its tab; the motion study then checks the
pump's leaves against each other and the engine at full load, since it takes
a flexible placement leaf by leaf.

**The bottom end's outside** is casting form read off the front photograph
and the factory section, not a drawing, and none of it touches a bore,
gallery, bearing or mating face. The block carries a raised panel over the
water jacket on each side with three core plugs in bosses between the bores,
a ledge along the pan rail, and on the pump side (+X) the interfaces what
hangs there sits on:

- **oil filter pad** — flat, round, Ø70, its face 12 proud of the crankcase
  wall (x 112) at the main gallery's height (z 60), with four tapped M8 holes
  on a 52 mm PCD at 45° and a Ø12 port drilled straight into the gallery. In
  y it is centred between the dipstick's run ahead of it and the engine
  mount behind (y −139), under the injection pump's front half: the oil
  filter's can has that gap and no more. Frame `oil_filter` on the block.
- **dipstick socket** — Ø22, in the first bay: its bore breaks into the
  crankcase behind the front main's bulkhead, 20 mm above the rail, and its
  top face (114, −198.5, −3.4) is on the dipstick's run ahead of the pump, so
  the socket leans 25° outward and about 18° forward. Frame `dipstick`
  (world-aligned, at the top face).
- **injection pump seat** — see [The injection pump](#the-injection-pump).
- **pump bracket threads** — two blind M8 in the rib over the second main,
  under the pump's rear lugs. Frame `pump_bracket`.

The numbers live in `om615_spec.py` (`OIL_FILTER_BOSS`, `DIPSTICK_BOSS`,
`DIPSTICK_AXIS`, `DIPSTICK_RUN_Y`). The sump is deep only under the front cylinders — a flat
well to the centre main, a 40° ramp, a floor 45 mm under the rail behind —
with the drain plug at the well's lowest point, a turned-down lip round its
flange and ribs between the bolts. The timing cover's outline follows the
case: full width over the crank and the lower bolts, a waist over the chain
runs, out round the water pump, and back to the cam cover's width at the
head (`COVER_SHAPE`; the rails measure their room off the drawn arcs). The
crank pulley is pulley, damper ring and hub with its bolt heads, on the same
groove, and the engine mounts are cast arms — a plate on the crankcase pad,
two triangular webs arched clear of the bolt heads, and a foot with a boss
for the rubber mount — on the same pad and mount hole as before.

Construction rules the builders keep:

- Mounting frames are world-aligned translations; only what something turns
  or slides on carries an axis. A mate aligns axes, so a frame whose z "points
  down" turns the part over.
- Everything is a drawn profile (`slab`, `disc`, `axis_solid`, `bore` in
  `om615_bottom.py`), because a part built from primitives as bodies of
  their own counts its overlaps twice.
- The head is built in one order: the water jacket is a void, so everything
  that must stay dry has its wall put back before it is drilled. The jacket is
  a ladder of channels and columns, not a slab crossing every port.
- Manifolds are built solid and bored afterwards, runners leaving each port on
  its own axis along analytic elbows, and every addition is checked to add
  volume (a fuse that loses material is a failure) and every bore to remove
  no more than its own tool.
- A rounded casting is hollowed by SUBTRACTING a second body — its cavity,
  drawn at the wall's inset and rounded about the same centres — not by
  cutting a square cavity into a rounded outside, whose corner would break
  through the wall. The cam cover's rear-end radius is derived from what
  stands in the cavity's corners (the last rocker post, the last cam cap).
- A rigid valve spring is drawn at its SHORTEST, so it never clashes.
- Bolt holes are the standard's own clearances; a tapped hole's major diameter
  is the bolt's nominal. Mounting datums belong to the part builders, so
  rebuilding a part cannot lose its assembly interfaces.

## The injection pump

The Bosch PES4M is `generators/pes_pump/`: its parts are built into this
project (the tabs go in an "Injection pump" folder) by `pes_pump.build_parts`,
assembled into the pump's own assembly ("PES4M injection pump") by
`pes_pump.assemble` with its load configurations (`pes_pump.configure`), and
placed in the engine as ONE **flexible** occurrence, "injection pump"
([flexible sub-assemblies](../../docs/assemblies/README.md#flexible-sub-assemblies)):
its mechanism moves in the engine's solve and studies, and the engine names
its parts and mates by path (`po.inner(id)`, `po.mate(mate)`). Its frame maps onto the engine's by one turn: pump +X (toward the
governor) is engine +Y, the rear; pump +Z is +Z; so pump +Y, the control
rack's side, faces the block and the feed pump and hand primer face out, as
in the photographs.

**Where it is.** The pump's interface is the pump's to state
(generators/pes_pump/README.md, "What an engine mounts it by") and
`om615_spec.py` reads it from `pes_spec.py`: flange face, register, bolt
circle and clocking, sprocket, timing device. The rest follows:

- **y**: the pump's sprocket runs in the timing chain's plane, so the flange
  face is at `DRIVE_Y + (FLANGE_FACE_X − CHAIN_PLANE_X)`, y −200, five
  millimetres behind the block's front face; the drive end is in the timing
  case and the governor points rearward.
- **z**: the element deck is level with the block's deck (`PUMP_Z = DECK −
  H_TOP`, 120.9), so the pump lies along the upper block with its delivery
  valves beside the lower head.
- **x**: the sprocket's tips (r 69.4) clear the chain case's cavity — the
  chain, both rails and the oil pump's drive are all inside it — by 4 mm at
  every height they span (`om615_bottom._pump_x`): x 154.6. The rack side
  and the lugs then stand 8 mm off the ribs and the governor 20 mm off the
  upper block.

**What it seats on.** The block grows a **seat ring** (r 50) on a web off its
front, between its front face and the seat, outside the chain case's gasket.
Its back face is the seat: the Ø60 register, then a bore round the timing
device (r 34.5), and three blind M8 on the pump's 78 PCD, one on top, as the
pump clocks them. The ring runs forward, through the timing cover, to a
millimetre short of the sprocket. The frame `injection_pump_mount` is on the
seat, z along the camshaft to the rear and x up, the twin of the pump's
`flange`. The pump's own register (Ø60) is smaller than its timing device's
hub (Ø68), so a hub could not pass the bore it locates in; the block's bore
steps out behind the register rather than the pump's numbers moving. The
**timing cover** grows a drum round the sprocket, as on the real engine: r
83.6 (big enough for the vacuum pump's flange as well), a cavity clear of the
sprocket's tips and open to the chain case, and its back wall bored for the
seat ring.

**How it is held.** Three ISO 7380 low-head M8, derived from their holes: rim
on the flange's back face, in a pocket the pump's housing has for each head,
to the thread's floor in the seat. A **bracket** (paint) on the rib over the
second main, two derived M8, carries the pump's rear lug on a shelf. The
control rack is held at full load by the pump's own "Full load"
configuration, which the placement runs under; the flyweights sit at zero
advance.

**The drive.** A chain relation from the crank's main mate to the pump
camshaft's revolute INSIDE the pump occurrence (`{path: [pump], mate}`), 22:44 (`CRANK_TEETH : SPROCKET_TEETH`), same sense, like
the camshaft's — and PHASED: the relation's `phase` (`pump_phase_deg()`,
52.89 pump degrees at crank zero) puts element 1's port closing, the start of
delivery (`pes_spec.valve_window(1)[0]`, where the plunger's top edge covers
the spill port), `SPILL_ADVANCE_DEG` = 24° crank before cylinder 1's
compression TDC. That TDC is read off the engine's own valve timing (the
exhaust lobe starts `EXHAUST_OPEN_CAM` after it), not assumed. **24° is an
estimate for an OM615 at the pump, not a sourced figure.** Element k feeds
cylinder k, so the pump's 1-3-4-2 is the engine's and every element is timed
alike.

**The vacuum pump** sits on the timing cover's drum in front of the
injection pump with its face cam on the pump camshaft's axis (the casting is
offset from its cam by `CAM_CENTER`), and turns with it 1:1, as the real one
is driven off the injection pump's timing device.

**The injection lines** (steel, 6 × 2, `bent_tube`) each run from element k's
`outlet` — the top of its delivery-valve holder — straight up, then in and up
over the head's side, and down into cylinder k's injector along the
injector's own axis to its `fuel_inlet`; both ends are read off the solved
assembly, so a line fits wherever the pump and head put them. A line runs
over the risers of the lines behind it, so line 2 turns in highest, then 3,
then 4 (`LINE_ORDER`). Two small clamp blocks hold the risers in pairs, 1 with
2 and 3 with 4; their bores are 0.1 mm over the tube (a bore at exactly the
tube's size is two coincident cylinders, which OCC's boolean does not
survive), so they are fastened to a line rather than measured touching it.

## What a full run measures

From the study poses, not the inputs: every piston's stroke 83.600 mm (read
at the crown, since an occurrence's position is its part ORIGIN, the gudgeon
pin); the squish at TDC; every valve's 8.0 mm lift on its own lobe; the
firing order 1-3-4-2, read off the camshaft (cylinders 1 and 4 share a throw,
so the pistons cannot say which fires; an exhaust lobe peaks a fixed crank
angle after its own cylinder's firing TDC); the alternator, fan and
vacuum-pump turns against their ratios; the vacuum-pump diaphragm's travel;
and the injection pump (`check_pump`, `pump-motion-report.json`): its
camshaft's turn per 720° from the study's own mate values (exactly 1.0);
every plunger's 7.0 mm stroke; the plungers peaking 1-3-4-2; and each
element's port closing 24 ± 0.5° before its own cylinder's compression TDC,
computed from the MEASURED phases of the pump's and the engine's camshafts
(exact) and the pump's port-closing law, not interpolated between samples a
quarter of a pump turn apart. Any of them off fails the run. The exact
standing sweep runs with a raised pair budget and fails if truncated — at its
defaults it stops early and would read as clean. The collision mesh's
deflection scales with the model and every running fit here would read as
contact, so the gates are exact.

## Sources

Reference target: the early **200 D/8, OM615.913** (21:1), not a mix of early
and late 200 D / 220 D figures.

| source | used for |
|---|---|
| [Workshop manual for Mercedes-Benz 200/220/230 & 250](https://documents.cdn.ifixit.com/SWZXI3IJg11tfP3v.pdf), printed pp. 23–24, diesel columns | 87 × 83.6 mm, 1988 cc, 21:1, firing order 1-3-4-2; valve heads (intake 38.8, exhaust 33.2, midpoints of 38.70–38.90 / 33.10–33.30), stem 9.92, spring wire 3.8 |
| [Proxmann technical catalogue](https://www.proxmann.com/upload/file/catalog/proxmann_teknik_katalog.pdf), printed p. 67, piston 71160800 (OM615.913/.915) | compression height 52.9, overall height 91.4, gudgeon pin 26 × 72, rings 3 / 2 / 4. Its `BD -63,3` entry is suspect and not used |
| Mercedes-Benz *Service Manual Engines 615, 616, 617.91*: 05.4-118/1 | machined head height 84.8–85.0 (84.0 is the refacing limit) |
| same: 05-100, 05-235 | valve arrangement, single-side rocker gear |
| same: 07-137, 07.1.4-230/1 | stepped nozzle holder and its 27 mm socket; inclined injector and pre-chamber with sealing plate |
| same: 14-470 | intake arches above the exhaust on the same side |
| same: 13-335 | 9.5 mm V-belt section |
| same: 05-412, 07-200 | intermediate shaft, injection timer and pump installation (no dimensioned mounting pattern) |
| owner's photographs, Daimler sectional drawing | external arrangement and casting form only; not scaled drawings |

Everything else is a modelling estimate: the casting exterior (panels, core
plugs, ribs, ledges, the sump's well and ramp, the cover's outline, the mount
arms), what hangs on it (both filters' sizes and places, the brackets, the
hose, pipe and injection-line routes, the water pump's inlet neck, the
dipstick's lean and height), the injection pump's place (derived from its
interface and the chain case, above) and its 24° spill timing, rod length,
the 3.2 mm crown-to-deck
gap, bowl and pre-chamber shape and volume, port routing, rocker and retainer
construction (the manual shows cap and lock nuts; the model uses collets),
seat insert diameters, skirt reliefs, the cam-cover bolt pattern and towers,
injector lengths, pre-chamber steps and angle, the injection pump's flange
(the pump generator's own estimate; not to be used to machine a mating
pump), pulley centres
and diameters (the 811.42 mm belt pitch length follows from them, not from a
part number), fan dimensions, the vacuum pump's casting, cam law and 2.4 mm
diaphragm stroke. The model does not demonstrate the 21:1 compression ratio.
Bearing, piston and pin fits use exaggerated clearances. Materials are the
library's steel/aluminium approximations.

## The top end's castings

What the top end looks like follows the photographs (front, both sides, top
and the KTG 9810 sectional drawing), scaled against the bore and bore pitch;
the numbers are in `om615_spec.py` under "the top end's castings" and none of
them moves anything or any interface.

- **Cam cover**: a crowned roof (an arc tangent to both shoulders), the rear
  end rounded in plan and elevation, a rim at the gasket that the gasket
  follows, the three-pointed star raised on a medallion, the oil filler near
  the FRONT just behind the chain dome — where the top photograph and the
  sectional drawing both have it — and a breather stub on the manifold-side
  shoulder. No firing-order lettering: the SDK draws no text.
- **Intake**: a rounded box plenum on the old log's axis, its ends rounded,
  fed by a throttle body on a short neck in the middle of its outer face
  (round bore, butterfly shaft across it in two bosses, lever and pin on the
  rear end), and four curved runners — two into each paired port, joined at
  the flange by a slot.
- **Exhaust**: four runners into a log with rounded ends, one outlet straight
  down from the middle on a three-ear flange with three studs.
- **Head**: a drafted rib the length of the manifold side under the cover
  rail, and at its front end the ear the thermostat housing bolts to (two M8,
  derived like every other fastener), standing forward only outboard of the
  timing cover.
- **Thermostat housing**: flange, chamber, cover, an upper-hose outlet
  pointing forward and up (frame `hose`) and a heater spigot out of the
  chamber's outboard side (frame `heater`). `thermostat_ports()` in
  `om615_top.py` says where both end; the housing and the hose and pipe on
  them are drawn from it.
- **Fuel filter boss**: a pad on the head's pump side at its front end
  (y −185, ahead of the first glow plug) with two blind M8 threads, its face
  at `FUEL_FILTER_BOSS`; frame `fuel_filter` on the head.

## Finish

`om615_finish.py` is one table of finishes and a second naming the part each
is given to, applied once the engine is built and by every component
builder. Colours are linear, as the renderer lights them. It is visual only
(`set_appearance`): mass, the BOM and FEM keep the material.

| finish | what | parts |
|---|---|---|
| `paint` | grey paint over iron: colour 0.10–0.12, roughness 0.5, **metallic 0** | block, head, timing cover, sump, water pump, mounts, alternator brackets, rear seal carrier, oil filter housing, fuel filter bracket, injection pump bracket |
| `aluminium` | bare sand-cast aluminium, light and dull | cam cover, intake, vacuum pump castings, alternator castings, thermostat housing |
| `cast iron` | as-cast iron that has been hot, near black | exhaust manifold |
| `rubber` | black, rough | fan belt, gaskets, both coolant hoses |
| `black` | black paint on pressed steel | fan and its spacer, pulleys, damper, filler cap, fuel filter |
| `steel` | bare drawn tube | heater pipe, dipstick tube, the four injection lines |
| `filter blue` | dark blue-black enamel, colour (0.010, 0.013, 0.030), roughness 0.3 | the spin-on oil filter |
| `yellow` | moulded plastic | the dipstick's handle |

The injection pump brings its own finish (generators/pes_pump/README.md#what-it-looks-like).

Paint is a dielectric. With any metallic in it the painted castings mirror the
lights — dark from one side and silver from the other — where the photographs
show one even satin grey. Fasteners, internals and anything the table does
not name keep their material's own look; the names are exact, so a renamed
part falls back to its material rather than picking up a stranger's finish.

## What hangs on it

Static parts, in `om615_accessories.py`, each drawn in engine space and hung
by a world-aligned frame that coincides with its holder's (`HANGING` lists
part, holder and both frames). The interfaces they sit on are in
`om615_spec.py` under "what hangs on it"; their own proportions are read off
the photographs against the 87 mm bore and 97 mm bore pitch. Each touches
its holder and nothing else: the `local` stage measures every one of those
contacts (distance zero, no overlap), and the standing sweep and the motion
study cover them like any other part.

- **Oil filter housing** (paint), on the block's pad: a flange as thick as
  the pad stands proud with four M8 clearance holes and the Ø12 port, bolted
  by four derived M8; a Ø30 neck straight out past the bolt heads and then
  down and out to a seat facing down and outward at 30° from vertical. The
  seat's place is derived twice over and the lower wins: its highest edge
  12 mm under the injection pump's lugs, and its inboard edge under the
  flange's own lower bolt heads.
- **Oil filter** (filter blue), the spin-on can on that seat, hanging down
  and outward below the pump: a Ø95 × 140 can (1.09 bores × 1.44 bore
  pitches) with its rolled seam and the boss a filter wrench takes. In y it
  fills the gap between the dipstick's run ahead of it and the engine mount
  behind, 5 mm each side.
- **Fuel filter** (black) on its **bracket** (paint): a plate on the boss at
  the head's front end held by two derived M8, and a shelf reaching out and
  forward to a ring over the vacuum pump that the filter's neck drops
  through. The filter head sits on the ring with its two banjo bosses and
  bleed screw; the tall Ø80 × 150 canister (0.92 bores × 1.55 bore pitches)
  hangs below it, high at the front, its drain screw 10 mm above the timing
  cover's pump drum (`FUEL_SHELF_Z` is derived from the drum and the vacuum
  pump's lid).
- **Upper coolant hose** (rubber): its bore is the thermostat outlet stub's
  outside and it starts at the stub's end; on along the stub's axis, then
  forward over the front of the engine to end in free air ahead of the fan
  (`RADIATOR_Y`), where a radiator's top tank would be.
- **Lower coolant hose** (rubber): off an inlet neck on top of the water pump
  housing (`WP_INLET`, the same hose size), which stands above the pulley and
  belt and ahead of the timing cover by the hose's radius. Up, across the
  front above the belt, forward at the radius that clears the fan's blade
  tips, and down to where a radiator's bottom tank would be.
- **Heater pipe** (steel, 16 × 1.5): off the thermostat housing's spigot, out
  past the plenum, and back along the manifold side at the height between
  the exhaust flanges and the plenum's floor, to past the head's rear face.
- **Dipstick tube** (steel) and **dipstick** (yellow handle): the tube's bore
  is the socket's; it leaves the socket along its lean, turns out under the
  pump's flange to its run AHEAD of the pump — between the timing cover's
  back face and the pump housing's drive-end wall, outboard of the seat ring
  — and rises straight up it to the head's top. The dipstick is a cap on its
  mouth, the blade's top inside it, and a flat ring facing the pump side, at
  the front.

Hoses and pipes are `bent_path` tubes: corners and one bend radius, as a
bender or a moulded hose has them (a hose bends round its own diameter, the
heater pipe round one and a half).

## Not modelled

The timing chain rerouted round the injection pump's sprocket and the
timing device's intermediate sprocket (the pump is driven by a relation; the
chain is drawn crank to camshaft); the fuel hoses (tank to feed pump, feed
pump to filter, filter to the pump's `fuel_inlet`) and the injectors'
leak-off pipes; equal-length injection lines (the real ones loop to match,
these take the shortest bent route); the governor's linkage to the
accelerator and its vacuum line; the vacuum pump's membrane, spring, valves
and pneumatics; the roller's own rotation; alternator terminals and field
windings (the stator windings are twelve packs, not turns); belt material
circulating; casting parting, cores, machining allowances and tolerances;
water and oil routing beyond the jacket and galleries (the thermostat
housing has no drilled feed: its ear is outside the head's side wall, and
the rocker chamber is behind that wall; the water pump's inlet neck stands
ahead of the volute and is not drilled into it); the radiator, the heater
valve and the hose clips (the hoses and the heater pipe end in free air);
the filters' elements and the dipstick's blade below the tube's mouth. A
clean interference report proves clearance of the modelled parts, not
completeness.
