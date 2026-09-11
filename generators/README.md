# generators

Parts that other CADs grow a workbench or a scripted-feature language for,
done here as ordinary Python against the API. Each is a program that makes
an ordinary parametric document: the dimensions a user adjusts afterwards
are project variables driving expressions, and anything computed (a tooth
profile) is frozen — change those inputs and run again.

Why this and not a scripting engine inside the app:
[../docs/capability-review-2026-08-31.md](../docs/capability-review-2026-08-31.md),
"scripted features" (2026-09-03). Short version: the regeneration loop
stays deterministic and free of interpreters, and a generator can loop and
branch in a real language with no sandbox.

**Layout.** A generator that is one file sits here; one that spans several
sits in its own directory beside them — `om615/`, `fpv_drone/`,
`cncest6090/`, `so101_gripper/` — with the same file names inside. Every
file adds BOTH its own directory and this one to `sys.path`, so a group's
modules import each other by bare name and still reach `_lib` and `gear`.

| generator | what | live afterwards | frozen |
|---|---|---|---|
| `gear.py` | spur gear: involute flanks as arcs, tip and root arcs, bore; `--backlash` thins the flanks (a nominal pair at nominal centres has none), `--phase` turns the pattern (what makes two mirrored sectors mesh instead of jamming tip to tip), `--sector` keeps teeth over a wedge and a hub elsewhere | thickness, bore | module, teeth, pressure angle, backlash, phase, sector |
| `gear_train.py` | two of them on a carrier, mated by their bores, coupled by a gear relation (docs/assemblies/relations-plan.md), checked for tooth clash over a pitch | the relation's ratio and phase | the gears |

Run one against the running app (it finds the one announced instance, or
takes `--port`; it never guesses):

    python3 generators/gear.py --module 2 --teeth 24 --thickness 8 --bore 6

`_lib.py` is the block every scratch script used to copy: sketch planes
that spell out the V axis, entity builders, an arc through three points, a
`check` that raises on a failed regeneration, and `connect`.

Gated: `tests/generators_smoke.py` (headless, in `ctest -LE gui`) builds the
default gear and checks it by volume, extents, entity count and that the
thickness still follows its variable.

Measured 2026-09-03, headless: m2 z24 (192 entities) 209 ms end to end; m1
z60 (360 entities) 449 ms, of which the sketch is 115 ms. Heavy sketches
are not a problem at gear sizes.

Not drawn: root fillets; undercut correction (below ~17 teeth at 20° the
flank runs radially from the base circle to the root, as an uncorrected
cutter would); helical, bevel, internal and rack forms. Each is a few
lines of the same arithmetic when a part needs it.

`fpv_drone.py` is a complete 5-inch FPV freestyle quadcopter: five carbon
parts to have cut, two to print, forty-two fasteners derived from the holes
they go in, and every bought component at its catalogue dimensions. The run
refuses to save what it has not checked — an empty exact interference sweep,
`check_fasteners` clean, seven measured clearances, and a motion study that
turns all four propellers through a blade pitch. It writes dimensioned
sheets with hole tables, STEP, STL, a BOM and a mass budget. Building it
found six app defects (five fixed, one worked around); see
[the model notes](../docs/modeling/fpv-drone.md) and `scratch/fpv-drone/BUILD.md`.

`watch/` is an automatic mechanical watch, time only with small seconds: a
21,600 vph movement (barrel, centre, third, fourth and escape wheels, a Swiss
lever escapement with a piped fit-spline hairspring, motion works under the
dial, a rotor winding the barrel through a reduction wheel, and a keyless
works — stem, clutch, winding pinion, setting wheel, setting lever and yoke —
so the crown winds the mainspring and sets the hands) in a 40 mm case with a
dial, applied indices and blued hands. It is built on this round's machinery
from the start: 86 dimensions are document variables (`watch_spec.declare`),
every one of its 37 occurrences is placed by a mate, 16 relations make one
motion study turn every hand at its true rate and the crown drive the ratchet,
and the clearances that decide whether it runs are DECLARED on the assembly.
The crown's two positions are CONFIGURATIONS of the assembly ("Crown in",
"Crown out"); the run gates on receipts, checks both, and refuses to save
what it has not checked. A clean mesh sweep — most of a full run —
is kept in `--out`'s `mesh-cache.json`, keyed on the generator's own files
and the app's source revision, so a run that changed neither does not sweep
the meshes again; `--recheck` forces it.

Its teeth are **cycloidal** (`watch/watch_gearing.py`), not `gear.py`'s
involute, and the gates are why: at 6–12 pinion leaves an involute train
interferes, a topped wheel loses continuous contact (the escape mesh fell to a
contact ratio of 0.92), and a profile shift that clears the 6-leaf escape
pinion leaves it pointed. `watch_spec.py` refuses at import any layout where a
wheel crosses a foreign arbor and any mesh below a contact ratio of 1.1. See
[the model notes](../docs/modeling/watch.md).

`cncest6090.py` starts a provisional CNC router layout and Z-axis fixture from
owner photographs. Only the 80 mm spindle diameter is owner-confirmed; the
remaining dimensions are editable project variables with recorded assumptions.
See [the model notes](../docs/modeling/cncest6090.md) for scope, measurement
priorities, JSON overrides and preview/local/full verification.

`so101_gripper/` is a parallel-jaw wrist gripper with a wrist camera for the
SO-ARM100/101 follower arm: eight printed parts, a 1:1 involute sector-gear
pair coupling two parallelogram fingers, and a 32x32 UVC camera aimed at the
grasp point. `so101_gripper.py` builds the parts, `_assembly.py` places them
at any link angle, `_motion.py` mates them and drives the mechanism through a
motion study, `_export.py` writes the deliverable. See
[the friction it found](../docs/rpc-friction-plan.md).

`toolmakers_clamp.py` is the dimensioned counterpart: an eight-part clamp based
on an author's published plan, with screw-driven closed assembly loops, exact
checks, STEP round-trip coverage, BOM and eight part sheets. See
[the source audit and workflow](../docs/modeling/toolmakers-clamp.md).
