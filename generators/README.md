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
`cncest6090/`, `so101_gripper/`, `smart_speaker/`, `watch/` — with the same file names inside. Every
file adds BOTH its own directory and this one to `sys.path`, so a group's
modules import each other by bare name and still reach `_lib` and `gear`.

| generator | what | live afterwards | frozen |
|---|---|---|---|
| `gear.py` | spur gear: involute flanks as arcs, tip and root arcs, bore; `--backlash` thins the flanks (a nominal pair at nominal centres has none), `--phase` turns the pattern (what makes two mirrored sectors mesh instead of jamming tip to tip), `--sector` keeps teeth over a wedge and a hub elsewhere | thickness, bore | module, teeth, pressure angle, backlash, phase, sector |
| `gear_train.py` | two of them on a carrier, mated by their bores, coupled by a gear relation (docs/assemblies/relations-plan.md), checked for tooth clash over a pitch | the relation's ratio and phase | the gears |

Run one against the running app (it finds the one announced instance, or
takes `--port`; it never guesses):

    python3 generators/gear.py --module 2 --teeth 24 --thickness 8 --bore 6

`smart_speaker/` is a two-way floorstanding loudspeaker PORTED from Onshape:
six MDF panels, four plywood coverings, two braces, a flared port, and the
drivers and amplifier as bought STEP. It is the one generator here that
started as somebody else's document, and most of what it is worth reading
for comes from that.

The parameter table arrived SIX times — a part-scoped copy in every wall —
and had already drifted, so it is declared once at project scope. The panels
each drilled their own dowels and the holes did not meet (side walls at
+/-126, the top wall at +/-110, the baffle none), so `joinery.py` makes the
JOINT the unit and both panels of one ask the same function where their
holes go; the carcass is then fastened along a tree by those holes rather
than placed at coordinates. The coverings are their own parts carrying a
`Derived` reference to the wall they cover, and take its outline from the
same `walls.silhouette()`, mitred 45 degrees where they meet.

**A frame built on ONE face has no meaningful X, and a fastened mate on it
has eight outcomes the solver picks between arbitrarily.** Bolting the
amplifier to the bay floor got, in turn, a case sunk 8 mm into its own floor
(the frame was at the bores' midpoint, which is mid-thickness of a 16 mm
panel), a case hanging under the floor (the base's normal points DOWN at it,
not up), and a case standing on edge 72 mm tall (the bought STEP's own
downward face was a 92 x 37 edge). The fix is to stop guessing which turn and
ask the app, which can see both frames: `asm.join` (`add_mate derive:"twist"`)
gives an exact joint turned the way the part is already placed, and refuses
rather than rounding when the two anchors are not square to one axis. What is
left in the generator is the ASSERTION each search used as its acceptance
test — the box is above the floor, inside the bay, lengthways — which now
fails loudly instead of being searched around. Where the bought geometry has
real mounting holes, use them at both ends: the amp's four are the pattern the
floor is drilled to, and the mate refuses if the case stops matching it.

`extract_bought.py` slices the 48 MB Onshape export into the ~2 MB actually
used, once; the output lives in `scratch/` and is regenerable, which is why
neither it nor the project is tracked. `--stage carcass` builds everything
that does not need those slices, and is what `tests/generators_smoke.py`
runs. What the run refuses to save without is in `verify.py`.

**Check an assembly with `asm.check()`, not by hand.** It gates the three
things that matter at once — nothing clashing, nothing FLOATING (one connected
component), and no near miss under a millimetre, which is almost always an
arithmetic slip. Contacts that are meant to happen go in its `allow` map
rather than into a loosened tolerance:

    asm.check(allow={("port tube", "port flare"): 80.0})

Written out by hand instead, a whole round shipped with eight T-nuts floating
in mid-air and a clash reader that returned `[]` because it asked
`interference` for a key that method did not have. `check_clashes()` is the
same gate without the contact distances.

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
lever escapement that swings — balance and fork driven by cams off the
escape wheel — with a piped fit-spline hairspring, motion works under the
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
