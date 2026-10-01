# generators

Parts, assemblies and whole products written as ordinary Python against the
app's API. Each is a program that makes an ordinary parametric document: the
dimensions a user adjusts afterwards are project variables driving
expressions, and anything computed (a tooth profile, a cam table) is frozen —
change those inputs and run again. A generator can loop and branch in a real
language, while the app's regeneration stays deterministic and free of an
embedded interpreter.

Run one against a running instance — your own scratch one
(`tools/headless-preview.sh --start <port>`), never the human's. Most find
the one announced instance or take `--port`; they never guess:

    python3 generators/gear.py --module 2 --teeth 24 --thickness 8 --bore 6

## Index

| generator | what | notes |
|---|---|---|
| `gear.py` | spur gear: involute flanks as arcs, tip and root arcs, bore | below |
| `gear_train.py` | three gears on a carrier and a rack on a rail, coupled by relations, checked for tooth clash over a pitch | below |
| `slider_crank.py` | block, crank, rod and piston closing a loop; a 360° study checks the stroke is 2R with no clash | `--crank`, `--rod` |
| `single_cylinder.py`, `aircooled_engine.py`, `carburettor.py` | a single-cylinder four-stroke on primitives, the same engine from sketches only, and its carburettor as a subassembly | [below](#single-cylinder-and-air-cooled-engines) |
| `toolmakers_clamp.py` | an eight-part toolmaker's clamp from a published drawing | [below](#toolmakers-clamp) |
| `print_coupon.py` | a printable coupon of bores, pins and threads at the print allowance table's sizes and ± steps, each engraved with its modelled diameter, to calibrate the table | [printed parts](../docs/tolerances/README.md#the-coupon) |
| `waveguide.py`, `waveguide_job.py` | a constant-directivity waveguide for the Alpair 7MS, and its 3-axis surfacing job on the 6090 | CAM |
| `rotary/` | a file handle and a barley-twist rolling pin, and their 4-axis jobs | CAM |
| `bike/` | a roadster bicycle modelled from photographs | [README](bike/README.md) |
| `chess/` | a Staunton chess set: turned pieces from rotary blanks, board, jig, and every CAM job | [README](chess/README.md) |
| `chassis/` | the FS chassis: a Formula Student space frame read from the team's SolidWorks CAD, and its shop order | [README](chassis/README.md) |
| `cncest6090/` | provisional layout and Z-axis fixture of a CNC router | [README](cncest6090/README.md) |
| `fpv_drone/` | a 5-inch FPV quadcopter, with sheets, loads and G-code | [README](fpv_drone/README.md) |
| `grill/` | a three-burner gas grill in brake-formed sheet | [README](grill/README.md) |
| `om615/` | the OM615 four-cylinder diesel | [README](om615/README.md) |
| `pantilt/` | a geared pan/tilt head for a 1.5 kg camera: two worm pairs, bearings, a machined base plate, printed PETG yoke | [README](pantilt/README.md) |
| `pes_pump/` | the Bosch PES4M injection pump for the OM615 | [README](pes_pump/README.md) |
| `smart_speaker/` | a two-way floorstanding loudspeaker cabinet | [README](smart_speaker/README.md) |
| `so101_gripper/` | a parallel-jaw wrist gripper for the SO-101 arm | [README](so101_gripper/README.md) |
| `watch/` | an automatic mechanical watch | [README](watch/README.md) |

## Layout and shared code

A generator that is one file sits here; one that spans several has its own
directory. Each file in a group puts its own directory on `sys.path`, so a
group's modules import each other by bare name; those that use `_lib` or
`gear` add `generators/` too, and `tools/` for the SDK.

`_lib.py` holds what scripts would otherwise each copy: sketch planes that
spell out the V axis, entity builders, an arc through three points, a
`check` that raises on a failed regeneration, and `connect`. Most newer
generators use the SDK (`tools/designer_sdk.py`) directly.

Conventions every generator here keeps:

- **The run refuses to save what it has not checked**, and gates on a
  check's RECEIPT, not its `ok`.
- **A part is finished when `check_geometry` says `verified`.** An invalid
  body poisons every later question, phantom clashes included.
- **Check an assembly with `asm.check()`**: nothing clashing, nothing
  floating (one connected component), no near miss under a millimetre, with
  contacts that are meant to happen in its `allow` map by exact pair and
  volume — `asm.check(allow={("port tube", "port flare"): 80.0})`. An
  allowance that matches nothing is itself an error. `asm.check_clashes()` is
  the same gate without contact distances.
- **Place by mates between named frames**, not by transcribed coordinates.
  Two frames mated need `flipped=False` (the RPC defaults to `true`), and a
  frame built on one face has no meaningful X — `asm.join` (`add_mate
  derive: "twist"`) takes the turn from the placement instead of guessing.
- **Dimensions carry their provenance**: sourced, read off a photograph,
  assumed, or a design choice, said on the line that holds them.

## gear.py and gear_train.py

`gear.py` makes a spur gear: involute flanks as arcs, tip and root arcs, a
bore. `--backlash` thins the flanks (a nominal pair at nominal centres has
none), `--phase` turns the tooth pattern (what makes two mirrored sectors
mesh instead of jamming tip to tip), and `--sector` keeps teeth over a wedge
and a hub elsewhere. Thickness and bore stay live variables; module, teeth,
pressure angle, backlash, phase and sector are frozen. `involute_profile()`
is importable, and other generators compose it into multi-level parts.

Not drawn: root fillets; undercut correction (below ~17 teeth at 20° the
flank runs radially from the base circle to the root, as an uncorrected
cutter would); helical, bevel, internal and rack forms.

`gear_train.py` mates pinion z12 → idler z24 → wheel z36 (module 2) by their
bores to a carrier, seats each on the plate by the mate's axial offset,
chains them with gear relations (ratios from the tooth counts, the idler
phased half a pitch), slides a rack by the wheel's pitch radius, and turns
the pinion through a tooth pitch checking for clash.

`tests/generators_smoke.py` (headless, in `ctest -LE gui`) builds the default
gear and checks it by volume, extents, entity count and that the thickness
follows its variable; it also builds the smart speaker's carcass.

## Single-cylinder and air-cooled engines

```sh
python3 generators/single_cylinder.py --port <port> [--out /tmp/single_cylinder] [--exact-drawing]
python3 generators/aircooled_engine.py --port <port> [--out /tmp/aircooled]
```

`single_cylinder.py` is a running engine on the SDK: bore and stroke 86 ×
86, a 150 mm rod, block, crank, rod, piston, head, two valves and a camshaft,
and four ISO 4762 head screws each derived from its hole. Every dimension is
a variable and every mate is between named frames. A chain relation drives
the camshaft at half crank speed and CAM relations lift the valves; a
49-sample study over 720° (one full valve cycle) measures the stroke and
lifts from the poses, re-checks with exact clash, and exports an animated
GLB and a three-view PDF. The block and head are plain boxes; only the head
is bolted.

`aircooled_engine.py` is the same mechanism from SKETCHES ONLY — no Box,
Sphere or Cylinder: a finned barrel (re-finned by changing `FIN_PITCH`), a
crankcase with a sump, a crank with mains and webs, a chain between an
18- and a 36-tooth sprocket, cam towers with open saddles and caps, valve
gear whose lobes are drawn from the relation's own lift function, intake,
exhaust and silencer, and `carburettor.py`'s carburettor placed as ONE
occurrence. The carburettor (Solex H-type style: bell mouth, fixed venturi,
butterfly, float bowl, jet, lever and return spring, plus a bellcrank and
link rod closing a four-bar throttle linkage) is built around its own origin
at the flange face, because a subassembly mates by its origin.

What these engines rely on in the app, and what building on them needs to
know:

- A closed loop HOLDS its driven DOF (`LoopPin`): a crank set to 90° stays at
  90° instead of being traded away as one more least-squares unknown.
- A cam table is interpolated by a periodic monotone cubic, which cannot
  overshoot its data; a spline that dips below zero in the dwell would drive
  a closed valve into the head.
- A motion study checks pairs that are rigid to each other once, exactly, at
  the first sample, and skips them after: a bolted joint that only touches is
  not a clash, and one that overlaps is reported (`rigid: true`); a scrub
  walks to its target at the study's own spacing, so a slider-crank cannot
  jump onto its mirror branch at 180°.
- Reading a hole diameter backwards through the clearance table is ambiguous
  for many sizes (11 mm is an M9 loose and an M10 normal), so it is refused
  naming the candidates and `fit_fastener` takes a `size`; a `Hole` drilled
  with a fit needs none.
- Exact HLR is priced by surface complexity: swept valve springs dominate a
  view, so the drawing is `quality: "fast"` unless `--exact-drawing`, and a
  view can leave occurrences out (`hidden`). A `Pipe` with a wall is tubing;
  a spring is solid bar.
- Overlapping loops in one sketch are refused, so a silhouette of
  overlapping shapes is one extrude per shape; the SDK's
  `Part.extrude_shapes()` does that loop. A slot's end cap reaches half its
  width past its end centre, and a sketch builder's `extent()` returns the
  profile's box to check before extruding. A radius on a drawn profile is the
  normal case, so the builder has `arc()` rather than leaving it to `slot`.
  Extrudes from the base planes go `xy` +Z, `yz` +X, `xz` −Y.
- `Part.islands()` counts the regions a section crosses — a boss hanging
  off a flange or a float bowl clear of its barrel shows as an extra island —
  and `Part.bored_through()` is its dual, a plane containing a passage's axis
  cutting an open duct into two lumps. The engines assert both where they
  matter.
- A part can be one solid, clash-free in every pose and correctly bolted
  and still be the wrong shape; looking is the check that sees it, so
  `Part.render(path)` screenshots one part, fitted (it needs a renderer, and
  a `--headless` instance refuses).
- `Part.bbox()` is the measured extent from `mass_properties`. `get_state`'s
  box is a cheap conservative bound that wraps a swept surface by its control
  points (a helical spring reads about sixty per cent over): right for a
  broad phase, wrong for a person.
- `modify_feature` takes `feature_id` as well as `index`, since an id is what
  every other call hands back; not `id`, which inside the spec already means
  the feature's own identity. `list_parts` returns `active_id` beside the
  `active` index, so a client caching the active tab can check it.
- A mesh containment test must not use a bounding box's centre: a cap
  straddling a shaft has its centre inside the shaft.
- In a four-bar, the coupler is turned end for end relative to the crank it
  drives, so its closing mate usually wants `flipped=True`; a loop that will
  not close is a geometry error, and seeding poses with `at=` only moves the
  start away from the answer.

## Toolmaker's clamp

```sh
python3 generators/toolmakers_clamp.py --port <port> [--out /tmp/toolmakers-clamp] [--preview]
```

An eight-part toolmaker's clamp from the [Journeyman's Workshop
drawing](https://journeymans-workshop.uk/images/projects/toolmkrs/toolmkrs.pdf)
and its [author's construction
account](https://journeymans-workshop.uk/toolclamps.php) (linked, not
vendored). Source dimensions: 75 × 12 × 12 mm jaws, a 30 mm taper to a 2.5 mm
nose, screw axes 8 and 38 mm from the heel, M6 × 1 studding 75 mm long, 11 ×
15 mm knobs and the captive knob's 8.2 mm groove. `D` in the script holds the
source dimensions, `CHOICES` ours, `SPACER` the author's prose; the written
`dimension-register.json` says which is which, and a null tolerance means
UNSPECIFIED.

Interpretations: the captive knob's blind bore goes through the flange end
(the detail appears to show it opposite, which conflicts with the assembly);
the author's salvaged M3 washer-head screw is replaced by an explicitly
dimensioned custom part; rod insertion (11 mm), the baseline 18 mm jaw
opening and the flat 5 mm rear tip are our choices; blind depths include a
118° drill point. Threads are cosmetic `Thread` features on tap-drill bores
(M6 on 5 mm, M3 on 2.5 mm) with bounded engagement, and five named
engagement gates check the intended threaded connections exist. Knurl and
chamfers are callouts only; steel generic, brass spacer.

The mechanism is PARALLEL ADJUSTMENT: a jaw slider keeps the jaws parallel,
and two closed loops with M6 screw relations (each referencing its `Thread`
feature, whose specification is authoritative) turn both screws as the
opening changes. A 41-sample study covers ten turns at quarter-turn steps,
18 to 28 mm. The full run writes the project, `toolmakers-clamp.step`,
`bom.csv`, eight SVG part sheets with associative dimensions and machining
notes, `dimension-register.json` and `report.json`; `--preview` skips
drawings and verification. `tests/toolmakers_clamp_smoke.py` re-imports the
STEP (eight solids, volume conserved), reopens the project and edits jaw
length and opening without re-running the generator, and computes both jaw
volumes independently from stock minus cutters. The checks are nominal
geometry, not tolerance stacks or loads.
