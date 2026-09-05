# The air-cooled single: sketches only

A second pass at the single-cylinder engine
(docs/modeling/single-cylinder-dogfood.md) with one rule: **no Box, Sphere or
Cylinder**. Every body starts as a drawn profile that is extruded or revolved.
Same mechanism — 86 × 86 bore and stroke, 150 mm rod, two valves on a 2:1
chain — so the kinematics were already proven and the round is about shapes.

    ./tools/headless-preview.sh <scratch-port>
    python3 generators/aircooled_engine.py --port <scratch-port>

| | v1 (primitives) | v2 (sketches) |
|---|---|---|
| build | 224 calls, 2.0 s | 317 calls, 1.6 s |
| motion study | 735 checks, 3.9 s | 550 checks, **0.69 s** |
| animated GLB | 10.8 MB (1.6 after the spring fix) | 2.0 MB |
| stroke | 86.000 | 86.000 |

More calls, as expected — a profile is more drawing than a primitive. The
motion study got **5.7× faster** because the parts are the shapes they should
be: a revolved piston has one lateral face where a cylinder-minus-three-grooves
has a dozen, and the collision mesh follows.

## What the round found

**A sketch refuses loops that overlap.** Drawing a rod as "big-end circle +
small-end circle + shank polyline" in one sketch is refused — *"profile loops
overlap or nest inconsistently"*. So a silhouette made of overlapping shapes
becomes one extrude per shape, each `mode="add"`. That is more calls and no
guessing about which loop was meant to be a hole; it is the right refusal, and
it is worth knowing before you design a profile around it.

**A slot's end cap is half its width past the centre it ends on.** The crank
web is a `slot` from the main journal to the pin, which is what a web actually
is. At 90 wide round a pin 43 up it reached z=85 — and the barrel's underside
is at 82. The motion study called `barrel x crank` at seven of 49 samples.
A disc centred on the axis (what v1 used) hides this class of error, because
the shape only grows where you are looking.

**Fins put material on a bolt axis.** The head bolts are on a 65 mm circle,
inside the deck flange but outside the barrel wall, so the first blind hole
bottomed out 3 mm into the topmost fin: *"the axis runs into barrel 31 mm
in"*. Adding a boss was not enough — the fins still cross the bolt line all
the way down. A real finned barrel is cast exactly the way the fix models it:
the fins stop either side of each through-bolt spigot.

**Four bolt bosses hanging in mid-air, and nothing complained.** The first
version relieved the fins with a d=38 cut at each bolt and then added a d=30
boss. The relief reaches r=46 and the boss only r=50, so it ate four
millimetres of the very wall the boss had to land on: four pillars hanging off
the deck flange with daylight behind them. Every check passed — one solid
body (the flange joins them at the top), the study clean, the bolts fitted —
because none of them asks "is this shape sensible". It took a person looking
at the part. The boss now runs the barrel's full height and overlaps the wall
by two millimetres, which is what a through-bolt spigot actually is, and the
fins run into it.

**A blind hole at the end of a truncated joint was refused** — and this one
was a defect, not geometry. The head bolts stop in a blind hole 30 mm into the
barrel, and their axis carries on 158 mm further into a crank web they do not
touch. `far_end` is the documented way to say where a joint stops, but the
blind-hole check consulted the raw stack's LAST segment rather than the last
one inside the joint, so it refused even with a far end given. The stack stays
untruncated on purpose (the washers and the nut live in it); the check now
asks the right question, and the refusal names `far_end` when there isn't one.
Fixed with a case in `tests/fastener_stack_smoke.py`. Same family as W1 in
docs/agent-scale-followup-plan.md: two pieces of code disagreeing about where
a joint ends.

## What it looks like

A finned barrel and head, four bolt bosses running the barrel's full height
with the fins running into them, shaped crank webs and counterweights, a turned
piston with a dished underside, a waisted rod, and egg-shaped cam lobes. Every
one of those came from a profile, and every dimension is still a variable —
change `FIN_PITCH` and the barrel re-fins itself.

The drawing is deliberately not part of this generator: an exact sheet of this
engine is dominated by its two valve springs, which is a known bug with its
own measurements (docs/known-bugs.md).
