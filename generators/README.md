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

| generator | what | live afterwards | frozen |
|---|---|---|---|
| `gear.py` | spur gear: involute flanks as arcs, tip and root arcs, bore | thickness, bore | module, teeth, pressure angle |
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

`cncest6090.py` starts a provisional CNC router layout and Z-axis fixture from
owner photographs. Only the 80 mm spindle diameter is owner-confirmed; the
remaining dimensions are editable project variables with recorded assumptions.
See [the model notes](../docs/modeling/cncest6090.md) for scope, measurement
priorities, JSON overrides and preview/local/full verification.

`toolmakers_clamp.py` is the dimensioned counterpart: an eight-part clamp based
on an author's published plan, with screw-driven closed assembly loops, exact
checks, STEP round-trip coverage, BOM and eight part sheets. See
[the source audit and workflow](../docs/modeling/toolmakers-clamp.md).
