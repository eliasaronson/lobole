# OM615 four-cylinder diesel

An OM615-shaped Mercedes diesel — the early 200 D/8, OM615.913: 87 × 83.6 mm,
1988 cc, five main bearings, a single overhead camshaft on a duplex chain
working the valves through rocker arms, and an inclined pre-chamber per
cylinder — built complete down to bearing shells, seals, oil galleries,
water jacket, manifolds, fasteners derived from their holes, the front
drive — belt, fan, alternator and vacuum pump — and what hangs on the
outside: oil and fuel filters, coolant hoses, the heater pipe and the
dipstick, in a factory finish. It is a clearance-checked
kinematic model, not factory or manufacturing geometry. The injection pump
that fits it is `generators/pes_pump/`, built separately.

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
  (49 samples by default — coarse steps can land a slider-crank on its other
  branch) and the measured strokes, lifts and firing order. `om615-unchecked.dsgn`
  is saved before validation so a failed gate leaves geometry to inspect;
  `om615.dsgn` only when accepted. `--build-only` and `--no-study` are the
  older names for `preview` and `local`.
- **Components.** `--component KEY` builds one part in isolation for shape
  iteration; `--update-component KEY --project P` rebuilds one part inside a
  saved engine through the component staging layer (`tools/designer_components.py`),
  keeping part, feature and occurrence identities and reusing unchanged
  geometry. Keys: `belt`, `fan`, `vacuum-pump` (the casting), `block`, `head`,
  `sump`, `timing-cover`, `damper`, `engine-mount-left`, `engine-mount-right`,
  `cam-cover`, `intake`, `exhaust`, `injector`, `prechamber`, `thermostat`,
  and what hangs on it: `oil-filter`, `fuel-filter`, `fuel-filter-bracket`,
  `upper-hose`, `lower-hose`, `heater-pipe`, `dipstick-tube`, `dipstick`. A
  component comes out with its finish (below), so a part rebuilt on its own
  looks as it does in the engine. A component's fingerprint covers the
  builder modules, the finish table and the SDK; a change to `om615_spec.py`
  or the shared accessory mounting inputs refuses a component update and asks
  for a full rebuild.
- **Outputs** in `--out` (default `/tmp/om615`): the project, an animated
  `om615.glb` (unless `--no-export`), `reference-dimensions.json` (measured
  B-Rep extents with their evidence status), `validation-report.json`,
  `standing-report.json`, `motion-report.json`, the accessory reports and
  `timings.json`.

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
| `om615_accessories.py` | belt, fan, alternator (stator and rotor subassemblies), vacuum pump and its timer cam, mounting brackets; what hangs on it — oil filter housing, fuel filter and its bracket, the coolant hoses and heater pipe, the dipstick and its tube |
| `om615_finish.py` | how each part looks: a table of finishes and the part each is given to |
| `om615.py` | assembly, fasteners, the component table, stages and checks |

Frame: +Z up and the bore axis (crank centreline z = 0, so a piston's height
is its z); +Y along the crank, FRONT (timing chain) at −Y; +X across, with
both manifolds on +X and the injection equipment on −X. Cylinder 1 is at the
front.

**The mechanism** has one driving DOF, the crank: four slider-crank loops off
one crankshaft; a chain relation at 1:2 to the camshaft; per valve a CAM
relation for its lift and a RACK relation that makes its rocker follow; the
front drive (alternator, fan, vacuum-pump timer at half speed) coupled to the
crank by relations on pulley pitch radii, assuming no slip. The belt occupies
its running envelope and does not circulate.

**The alternator** is two rigid child assemblies — stator and rotor — joined
by a revolute in the engine, because a nested assembly is rigid in the solver
and a turning rotor needs its DOF exposed in the parent. The whole-engine
sweep checks top-level pairs only, so the children are swept on their own.

**The bottom end's outside** is casting form read off the front photograph
and the factory section, not a drawing, and none of it touches a bore,
gallery, bearing or mating face. The block carries a raised panel over the
water jacket on each side with three core plugs in bosses between the bores,
a ledge along the pan rail, and on the pump side (−X) the two interfaces the
oil filter housing and the dipstick tube sit on:

- **oil filter pad** — flat, round, Ø70, its face at (−112, 48.5, 60) with
  normal −X: in the bay between the ribs over mains 3 and 4, at the main
  gallery's height, with four tapped M8 holes on a 52 mm PCD at 45° and a
  Ø12 port drilled straight into the gallery. Frame `oil_filter` on the block.
- **dipstick socket** — Ø22, its top face centred at (−114, 145.5, 32.5), axis
  (−sin 15°, 0, cos 15°) leaning outward; a Ø10 bore runs down it into the
  crankcase 20 mm above the rail, in the bay between mains 4 and 5. Frame
  `dipstick` (world-aligned, at the top face).

The numbers live in `om615_spec.py` (`OIL_FILTER_BOSS`, `DIPSTICK_BOSS`,
`DIPSTICK_AXIS`). The sump is deep only under the front cylinders — a flat
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
  `om615_bottom.py`), because primitives are always new bodies and a part
  built from them counts its overlaps twice.
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

## What a full run measures

From the study poses, not the inputs: every piston's stroke 83.600 mm (read
at the crown, since an occurrence's position is its part ORIGIN, the gudgeon
pin); the squish at TDC; every valve's 8.0 mm lift on its own lobe; the
firing order 1-3-4-2, read off the camshaft (cylinders 1 and 4 share a throw,
so the pistons cannot say which fires; an exhaust lobe peaks a fixed crank
angle after its own cylinder's firing TDC); the alternator, fan and timer
turns against their ratios; the vacuum-pump diaphragm's travel. The exact
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
arms), what hangs on it (both filters' sizes and places, the bracket, the
hose and pipe routes, the water pump's inlet neck, the dipstick's lean and
height), rod length, the 3.2 mm crown-to-deck
gap, bowl and pre-chamber shape and volume, port routing, rocker and retainer
construction (the manual shows cap and lock nuts; the model uses collets),
seat insert diameters, skirt reliefs, the cam-cover bolt pattern and towers,
injector lengths, pre-chamber steps and angle, the injection-pump register
and bolt pattern (not to be used to machine a mating pump), pulley centres
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
- **Fuel filter boss**: a pad on the head's pump side between cylinders 1 and
  2 with two blind M8 threads, its face at `FUEL_FILTER_BOSS`; frame
  `fuel_filter` on the head.

## Finish

`om615_finish.py` is one table of finishes and a second naming the part each
is given to, applied once the engine is built and by every component
builder. Colours are linear, as the renderer lights them. It is visual only
(`set_appearance`): mass, the BOM and FEM keep the material.

| finish | what | parts |
|---|---|---|
| `paint` | grey paint over iron: colour 0.17–0.19, roughness 0.5, **metallic 0** | block, head, timing cover, sump, water pump, mounts, alternator brackets, rear seal carrier, oil filter housing, fuel filter bracket |
| `aluminium` | bare sand-cast aluminium, light and dull | cam cover, intake, vacuum pump castings, alternator castings, thermostat housing |
| `cast iron` | as-cast iron that has been hot, near black | exhaust manifold |
| `rubber` | black, rough | fan belt, gaskets, both coolant hoses |
| `black` | black paint on pressed steel | fan and its spacer, pulleys, damper, filler cap, fuel filter |
| `steel` | bare drawn tube | heater pipe, dipstick tube |
| `yellow` | moulded plastic | the dipstick's handle |

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
  by four derived M8; a Ø30 neck inside the bolt circle, long enough that the
  canister clears the bolt heads; and the canister standing beside the
  crankcase — cast head, a Ø90 × 150 bowl (1.03 bores × 1.55 bore pitches),
  lid and centre bolt — its top near the deck.
- **Fuel filter** (black) on its **bracket** (paint): a plate on the head's
  boss held by two derived M8, and a shelf reaching out with a ring the
  filter's neck drops through. The filter head sits on the ring with its two
  banjo bosses and bleed screw; the Ø80 × 110 canister (0.92 bores × 1.13
  bore pitches) hangs below it, its bottom 10 mm above the injection pump's
  mounting flange (`FUEL_SHELF_Z` is derived from that flange).
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
  is the socket's; it leaves the socket along its 15° lean far enough to
  clear the rail's ledge, bends upright and runs beside the block, clear of
  the fourth glow plug, to the head's top. The dipstick is a cap on its
  mouth, the blade's top inside it, and a flat ring facing the pump side.

Hoses and pipes are `bent_path` tubes: corners and one bend radius, as a
bender or a moulded hose has them (a hose bends round its own diameter, the
heater pipe round one and a half).

## Not modelled

The fuel lines and the pump's drive (the timing device, its intermediate
sprocket and the rerouted chain); the vacuum pump's membrane, spring, valves
and pneumatics; the roller's own rotation; alternator terminals and field
windings (the stator windings are twelve packs, not turns); belt material
circulating; casting parting, cores, machining allowances and tolerances;
water and oil routing beyond the jacket and galleries (the thermostat
housing has no drilled feed: its ear is outside the head's side wall, and
the rocker chamber is behind that wall; the water pump's inlet neck stands
ahead of the volute and is not drilled into it); the radiator, the heater
valve and the hose clips (the hoses and the heater pipe end in free air);
the filters' elements and the dipstick's blade below the tube's mouth. The
injection pump is not fitted here (it is `generators/pes_pump/`), so nothing
checks the oil filter canister, which stands where the pump's rear end
would, against it. A clean interference report proves clearance of the
modelled parts, not completeness.
