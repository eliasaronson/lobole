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
| `print_coupon.py` | a printable coupon of bores, pins and threads at the print allowance table's sizes and ± steps, each engraved with its modelled diameter, loose discs for the sideways print-in-place gap, and rings on the bed round posts with 45° heads: an air row for the vertical air a face needs over another part (whole layers) and a small-gap row for the gap at the post's size, to calibrate a print profile | [printed parts](../docs/tolerances/README.md#the-coupon) |
| `waveguide.py`, `waveguide_job.py` | a constant-directivity waveguide for the Alpair 7MS, and its 3-axis surfacing job on the 6090 | CAM |
| `rotary/` | a file handle and a barley-twist rolling pin, and their 4-axis jobs | CAM |
| `bike/` | a roadster bicycle modelled from photographs | [README](bike/README.md) |
| `chess/` | a Staunton chess set: turned pieces from rotary blanks, board, jig, and every CAM job | [README](chess/README.md) |
| `chassis/` | the FS chassis: a Formula Student space frame read from the team's SolidWorks CAD, and its shop order | [README](chassis/README.md) |
| `cncest6090/` | provisional layout and Z-axis fixture of a CNC router | [README](cncest6090/README.md) |
| `fpv_drone/` | a 5-inch FPV quadcopter, with sheets, loads and G-code | [README](fpv_drone/README.md) |
| `grill/` | a three-burner gas grill in brake-formed sheet | [README](grill/README.md) |
| `om615/` | the OM615 four-cylinder diesel | [README](om615/README.md) |
| `engine/` | a printed inline four: a flat-plane crank in a split block, rods with lapped caps on pressed pins, an overhead camshaft printed as two keyed halves lying on their cut, on buckets, valve stems sized for the height they carry standing, teardrop wrist-pin holes, TPU bellows valve springs and a printed TPU timing belt at 2:1, flywheel marks and a hand crank; the bore searched for two PLA plates, a 720° study with the valve timing, firing order and spring squeeze read off its traces, valve-to-piston requirements, the belt checked at every tooth, the wrist pin's strength across its layers, and negative controls that must fail (a cam a tooth off, rods 10 % long); predicts the swept volume, compression ratio, lift and valve events | [README](engine/README.md) |
| `pantilt/` | a geared pan/tilt head for a 1.5 kg camera: two worm pairs, bearings, a machined base plate, printed PETG yoke | [README](pantilt/README.md) |
| `counter/` | a print-in-place four-digit counter: number wheels, a Geneva carry between each pair that locks the next wheel between carries, a housing with a window per digit; counted and locked in motion studies | [README](counter/README.md) |
| `logo/` | the Loböle mark, read from its master SVG: branded into a pine log end and raised on the icon's tile, path-traced; `brand.py` makes the 2D files (icons, `.ico`, lockups, social preview) | [assets/brand](../assets/brand/README.md) |
| `hinge_box/` | a print-in-place hinged box: box and lid flat side by side on the bed, teardrop knuckles on 45° cone pins whose sockets leave the stacking air, swung 0-180° onto the rim in a motion study, with negative controls that fail stacking | [README](hinge_box/README.md) |
| `strandbeest/` | Theo Jansen's walker printed: twelve legs on a crankshaft of discs joined by crank pins printed lying down, every pin in a load path studied as it prints under the walker's own loads (its weight and the floor's friction), every joint a headed pin with a pressed cap, the bars in layers searched so nothing crosses; a 2° study proves the branch, the clash, the layers' air, the feet against Jansen's curve and the centre of mass over the feet, with negative controls; predicts the distance per crank turn | [README](strandbeest/README.md) |
| `planetary/` | a print-in-place herringbone planetary gear set: one plate off the bed already turning, its gaps from the print allowance table, proved by an assembly's motion study | [README](planetary/README.md) |
| `hook/` | a PLA wall hook sized for a load, printed flat and upright, with a predicted breaking load for each to test on a luggage scale | [README](hook/README.md) |
| `clock/` | a printed weight-driven pendulum clock: a Graham deadbeat escapement animated by a hidden beat (intermittent + cam) and proved by the clash check with a too-deep pallet that must clash, a cycloidal two-stage train (`cycloidal.py`, shared with the watch) checked for its ratios with a wrong tooth count that must fail, a click on the cord drum, plates on threaded pillars, the arbors and pillars printed lying on whole-layer flats, a compound pendulum sized from the modelled masses, FEM of the rod and of the arbor across its layers (its seat a region of a face), and every print plate's `check_print`; staged as an escapement test frame and the full movement | [README](clock/README.md) |
| `jar/` | a screw-top jar: PLA jar and lid with modelled 45° threads at the print allowance, a TPU gasket (printed in the TPU table's process) squeezed 25 % at the lid's stop, checked in an assembly with negative controls | [README](jar/README.md) |
| `pes_pump/` | the Bosch PES4M injection pump for the OM615 | [README](pes_pump/README.md) |
| `smart_speaker/` | a two-way floorstanding loudspeaker cabinet | [README](smart_speaker/README.md) |
| `so101_gripper/` | a parallel-jaw wrist gripper for the SO-101 arm | [README](so101_gripper/README.md) |
| `watch/` | an automatic mechanical watch | [README](watch/README.md) |

## Layout and shared code

A generator that is one file sits here; one that spans several has its own
directory. Each file in a group puts its own directory on `sys.path`, so a
group's modules import each other by bare name; those that use `_lib` or
`gear` add `generators/` too. The SDK is the `lobole` package, found
through `PYTHONPATH` (the nix shell and ctest set it), never by path.

`_lib.py` holds what scripts would otherwise each copy: sketch planes that
spell out the V axis, entity builders, an arc through three points, a
`check` that raises on a failed regeneration, `connect`, and
`fresh_project` for a generator's `--fresh`: it discards an untitled
scratch project, saying so, and refuses one with a path and unsaved work,
which is somebody's; for printed parts `check_print` (its answer waited
for as long as a plate of engraved faces takes) and `print_receipt`, `bed_zones` and `clear_x` (the
profile's keep-out zones, which every plate packer keeps clear of), and
`export_print_job` and `job_receipt`
([print jobs](../docs/tolerances/README.md#print-jobs)): a job of several
plates takes its slicer config from `$LOBOLE_SLICER_CONFIGS/<filament>.json`
(`tools/bambu-config.py` makes them) and without one is not written, the
receipt saying why. `pin_strength` studies a printed pin as it prints and
stops a run under a factor of 2, across its layers or against the yield
([printed layers](../docs/simulation/README.md#printed-layers)). Most newer generators use the SDK
(`python/lobole/sdk.py`) directly.

What a generator makes goes to the untracked `scratch/<name>/`, or under
`$LOBOLE_SCRATCH` when that is set (the tests below run each in a private
directory that way); those with an `--out` take that instead.

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

## The big generators as tests

A core change can break a generator that no unit test covers: a whole
generator puts hundreds of features, mates and checks in one document. So
the big ones run as tests, each its cheapest stage that still checks, from
nothing, against its own instance (`tests/smoke_app.py`: private
`XDG_STATE_HOME` and config) and writing to a private directory, never
`scratch/`:

    tools/ctest -C generators -L generators -j2      # ~6 min

<!-- generator-tests: made by tools/generator_tests.py -->
| test | runs | measured alone | budget |
|---|---|---|---|
| `generator_om615` | `om615.py --stage preview` (headless) | 221 s | 420 s |
| `generator_pes_pump` | `pes_pump.py --no-still`: the build, the standing sweep, the four studies and their measured strokes, order and turns (offscreen) | 44 s | 180 s |
| `generator_chess` | `run_all.sh --only board`: every design stage, then the board's CAM job verified and posted (offscreen) | 140 s | 360 s |
| `generator_planetary` | `print_coupon.py --seed` and `--record --z-gap-layers 2` (K5's measured air), then `planetary/build.py --no-render --profile "K5 PLA on P1S"` (every `check_print` item, stacking against the measured air and `bed_fit` on the P1S's bed, and the print job), then `--diameter 90` on the built-in table (its gap needs more than 70 mm): each the print plate, its exact mesh clearances and the STL's five shells, the assembly, both motion studies and the push check (headless) | ~470 s | 600 s |
| `generator_hook` | `hook/build.py`: both prints checked on the bed, the study at two meshes read for both orientations, the test card (headless) | 13 s | 60 s |
| `generator_jar` | `jar/build.py --no-render`: the three parts with modelled threads, the thread's phase found by interference, the squeeze measured twice, both seals as requirements, the lid and gasket screwed on as an `assembly_sequence` requirement, the unscrewing study, the four negative controls and the STLs (headless) | 83-198 s | 400 s |
| `generator_counter` | `print_coupon.py --seed` and `--record --z-gap-layers 2` (K5's measured air), then `counter/build.py --no-render --profile "K5 PLA on P1S"`: every `check_print` item of the plate (stacking and `bed_fit` included) and its print job, the negative controls (the old stacked layout among them), both counting studies with their readings, every lock both ways (headless) | ~310 s | 600 s |
| `generator_hinge_box` | `hinge_box/build.py`: the plate's every `check_print` item, stacking included, the swing 0-180° and both negative controls (headless) | 3 s | 60 s |
| `generator_strandbeest` | `print_coupon.py --seed`, then `strandbeest/build.py --profile "K5 PLA on P1S"` (the walker packs into the profile's bed): both stages, every check on the leg pair and the walker (solved, no branch jump and the knees, the exact clash with every cap's printed press listed, the layers' air, the feet, the stability), the stage-1 card's order checked piece by piece on the leg pair put together (a pose step turning the crank for the 15° caps), both negative controls failing, every print plate's `check_print` items, `bed_fit` among them, and every pin study at a factor of 2 (headless) | 2040 s | 3600 s |
| `generator_clock` | `print_coupon.py --seed`, then `clock/build.py --profile "K5 PLA on P1S" --no-render` (the period is the printed pendulum's, which needs a profile's layer and line width): every part verified, the escapement's studies in both stages with every beat's lock, the ratios, every mesh exact, the click, both stages' assembly sequences (stage 2's third and escape arbors going in together, as one group), every part put on in order against everything placed before it and each stage's assembly_sequence requirement passing, the negative controls failing (the first stage-1 escape arbor's insertion among them), stage 1's plate 1 as first printed (its pillars and arbors standing) failing `check_print`'s slender item on exactly those six, both FEM studies (the arbor's interlayer factor at least 2), every print plate's `check_print` and the period stated at the pendulum's infill (headless) | 641 s | 1200 s |
| `generator_engine` | `engine/build.py --no-render` on the seeded P1S profile: the bore search, every part verified, the 720° study with its press fits, the timing, firing order and springs off its traces, the belt at every tooth, the wrist pin's interlayer factor at least 2, both negative controls failing, the assembly sequence, the valve-to-piston requirements and both studies clean as requirements, every plate's `check_print` (the TPU plate on the seeded TPU profile), and the saved project reopened with every requirement passing as it lies (headless) | 127 s | 420 s |
| `generator_watch` | `watch/watch.py --stage assembly`: the parts and the assembly with every check `verify()` makes, the exact mesh sweeps included (headless) | 316 s | 660 s |
| `generator_pantilt` | `run_all.sh --skip yoke_topo renders` with `PANTILT_SAMPLES=6`: every part, the assembly with its exact sweeps and studies, the CAM job, the drawings (offscreen) | 181-349 s | 720 s |
<!-- generator-tests: end -->

A test passes when the generator exits 0 AND its own success marker is in
its output (om615's `preview passed`, the pump's GLB and screenshots, the
chess and pan/tilt logs' final `done`, the pan/tilt's `every setup ok and
complete`): exit 0 without the marker means it stopped early. Past its
budget the generator's process group is killed and the test says so; a
failure keeps the output directory and names it, with the generator's last
lines.

**Each one declares itself in its own directory**, `<name>/test.json`: its
order in the table, why it runs what it runs, its steps (argv lists run in
order with `&&`; `{python}`, `{generators}`, `{port}` and `{out}` filled in),
its success markers, a log of its own if it writes one, its instance
(`headless`, or `offscreen` when it renders), any extra environment, its
budget, and this table's cells. `tests/generator_runs.py` runs them,
`tests/CMakeLists.txt` registers one test per file, and
`tools/generator_tests.py` writes the table above between its markers
(`generator_table_gate` fails when it was not rerun). So a branch adding a
generator test adds a file and touches no shared list, which is where three
of five merges used to collide. An unknown key or instance kind, or a
missing budget, is refused, naming what is accepted.

They are registered for the ctest configuration `generators` only, so a
plain `ctest` -- the headless suite, CI, the nix sandbox -- never runs them,
and labelled `generators` and `slow`. `tools/merge-branch.sh --full` runs
them after the headless suite, two at a time.

Not covered: the pan/tilt yoke's statics and topology (`yoke_topo`, 9
minutes, its two projects 160 and 240 MB with every study's result saved
in them), and the chess set's
other CAM jobs (six more minutes). A run from nothing resolves every name
by this binary, so it catches a generator that picks by a name the app no
longer makes; it cannot catch an OLD document whose stored names no longer
resolve, since no fixture from an older binary is kept.

## gear.py and gear_train.py

`gear.py` makes a spur gear: involute flanks as arcs, tip and root arcs, a
bore. `--backlash` thins the flanks (a nominal pair at nominal centres has
none), `--phase` turns the tooth pattern (what makes two mirrored sectors
mesh instead of jamming tip to tip), and `--sector` keeps teeth over a wedge
and a hub elsewhere. Thickness and bore stay live variables; module, teeth,
pressure angle, backlash, phase and sector are frozen. `involute_profile()`
is importable, and other generators compose it into multi-level parts. It
takes `addendum` and `dedendum` in modules (1.0 and 1.25 by default): an
INTERNAL gear is cut by the outline with them swapped and a negative
backlash, which is how `planetary/` makes its ring, and a stub tooth is a
shorter addendum. A helical or herringbone gear is the outline extruded
with a twist: `twist_deg` is the helix's turn over the face width, `tan(helix)
/ r_pitch` radians a millimetre, and `herringbone: true` turns it back over
the second half ([twisted extrudes](../docs/modeling/README.md#twisted-extrudes)).
A pair's exact clearance comes out about 5 µm under the gap the backlash
asks for at module 1.4, the twist and a loft through turned sections alike,
so the shortfall is the profile's (its flanks are arcs, not involutes), not
the sweep's.

Not drawn: root fillets; undercut correction (below ~17 teeth at 20° the
flank runs radially from the base circle to the root, as an uncorrected
cutter would); bevel and rack forms.

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
