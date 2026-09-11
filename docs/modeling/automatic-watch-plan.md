# An automatic mechanical watch — plan

A generator, `generators/watch/`, for a complete self-winding watch: a
time-only movement (hours, minutes, small seconds — no date), a case to put it
in, and a dial, hands and rotor worth looking at. It is also the first model
built on this round's machinery from the start rather than converted onto it,
which is the point of building it now:

| machinery | how the watch uses it |
|---|---|
| `Param` / `declare()` (W1) | every dimension in `watch_spec.py` is a project variable; tooth counts are integers and stay out, as the drone's spec does |
| declared clearances (W3, v100) | the gaps that decide whether it runs — hands to dial, hands to each other, hands to crystal, rotor to ratchet, rotor to caseback — live in the assembly |
| receipts (W2) | the build gates on `standing`, not on `ok`: check_geometry per part, the exact sweep at rest, the motion study |
| `placed_by_mates` | every moving part is mated; the target is that nothing in the movement holds a typed transform |
| gear relations | the whole going train and the motion works are coupled, so one motion study turns every hand at its true rate |

## The movement

21,600 vph (3 Hz), the rate of most vintage and many current calibres.
Module 0.10 mm throughout, and **cycloidal teeth** (`watch_gearing.py`): each
gear's addendum an epicycloid, its mate's flank the hypocycloid of the same
rolling circle, which at half the pinion's pitch radius is a straight radial
leaf. Involute teeth cannot run at these pinion counts — see "Why cycloidal"
below. Checked by script before any of it was written here, including a sign
error in the first placement formula:

| arbor | wheel | pinion | meshes | centre distance |
|---|---|---|---|---|
| barrel | 96 | — | centre pinion | 5.40 |
| centre (2nd, 1 turn/h) | 80 | 12 | third pinion | 4.50 |
| third | 75 | 10 | fourth pinion | 4.25 |
| fourth (1 turn/min) | 72 | 10 | escape pinion | 3.90 |
| escape (1 turn/5 s) | 15 club teeth | 6 | pallet | — |

Centre distances are the exact pitch distances m(z₁+z₂)/2 — cycloidal action
does not forgive a spread centre — and the backlash is in the teeth instead: a
wheel tooth takes 0.47 of the pitch and a pinion leaf 0.42.

centre→fourth = 80/10 × 75/10 = **60**; fourth→escape = 72/6 = 12, so the
escape wheel turns in 5 s and its 15 teeth give 30 beats in 5 s = **21,600
vph**; the barrel turns once in 8 h.

**The fourth wheel is 72/6, not the obvious 84/7.** Both give 12:1, but an
84-tooth wheel has a 4.30 mm tip radius and sits 4.25 from the third arbor, so
it would sweep through that arbor. A wheel may not pass over any arbor except
the one whose pinion it drives; the layout script checks every wheel against
every arbor and every other wheel's plan and depth.

Motion works, under the dial: cannon pinion 10 → minute wheel 30, minute
pinion 8 → hour wheel 32 = **12:1**. Both pairs share the same two axes, so
their tooth SUMS must be equal (40 and 40); 12→36 / 10→40 also gives 12:1 and
cannot be built.

Hands: meshing phases turn every driven arbor by up to a tooth pitch — 22
minutes of hour hand on a 32-tooth hour wheel — so each hand is fastened at the
angle that cancels its arbor's resting dof, and the watch reads 12:00:00 with
the barrel at zero.

Plan (mm, movement centre at the origin, 3 o'clock = +X): centre (0, 0),
third (−2.252, −3.896), fourth (0, −7.5) — so the small seconds sits at 6 —
escape (2.237, −4.305), pallet (5.532, −4.486), barrel (3.471, 4.137),
balance (7.6, −4.6). Movement
Ø26. The pallet arbor is 3.3 from the escape wheel (1.35× its radius) so the
anchor arms have room, and it is SHORT — it pivots in a pallet bridge under the
balance, which is how the balance rim passes over it.

Depths are staggered so that wheels overlapping in plan never share a level:
barrel 0.30–2.30 below the mainplate's bridge face, centre wheel 2.70–2.95,
third 1.40–1.65, fourth 2.00–2.25, escape 1.00–1.20, balance 2.60–3.20, each
pinion spanning the level of the wheel it is driven by. Bridges above that,
the ratchet and crown wheel on the barrel bridge, the rotor over everything.

**Why cycloidal.** The first build used involute teeth from `gear.py`, and
the gates refused it three ways. Unshifted, eight of the nine meshes interfere:
below ~17 teeth the wheel's tip passes the interference point into a pinion
flank drawn radial below its base circle. Topping the wheels clears that and
leaves the 6-leaf escape mesh at a contact ratio of 0.92. A profile shift
clears it too, but the 6-leaf pinion is POINTED from x 0.42 and still
interferes below x 0.53 — there is no shift that is both. That is the reason
watch trains are cut cycloidal, found from first principles by the checks. The
gearing is designed per tooth SET: every dedendum is traced by half its own
pitch radius (radial) and every addendum by its mate's; the ratchet, the one
gear meshing two mates with the same teeth, forces the crown wheel's dedendum
onto the reduction pinion's rolling circle, which keeps both pairs conjugate.
Contact ratios 1.36–2.15, and the 72/6 escape mesh swept exactly through a
pinion pitch shows no interference, its flanks 13–17 µm apart.

**Escapement.** Swiss lever: 15-tooth club escape wheel, pallet fork with two
stones, balance with three arms, roller and impulse pin, and a hairspring made
as a fit-spline Archimedean spiral piped in round wire (there is no spiral
primitive). Its motion is intermittent, which a constant-ratio relation cannot
express, and a cam relation only drives a SLIDING follower — so the escapement
is placed, not animated: the pallet and balance turn on revolute mates of their
own, the stones sit on the escape wheel's tip circle rather than in lock, and
the pallet is cut clear of the escape wheel by construction, so it is clash-free
at any phase of the train.

**Winding.** The rotor drives the ratchet wheel through one reduction wheel,
and the crown wheel turns with the ratchet — one chain of relations rooted at
the rotor. A real rotor winds both ways through a reverser, which is left out:
unidirectional winding is the same visible train. The stem and crown are
placed and turn freely; the winding pinion's link to the crown wheel is a
contrate pair at right angles, which is not modelled.

## The watch

Ø40 steel case with lugs at 12 and 6, bezel, crown at 3, and a display
caseback. Dial Ø30 with applied indices and a sunk small-seconds register at 6;
blued hour, minute and seconds hands. Rhodium-grey bridges, a gold rotor
weight on a skeletonised rotor, a ruby jewel in every bridge pivot, blued
screws let in flush, perlage on the plate and Côtes de Genève on the
bridges.

There is no ruby, sapphire or blued-steel material, so those are appearance
overrides (`set_appearance`: colour, roughness, metallic) on real material
slugs, and mass comes from the slug.

**Glass renders opaque** — the ray tracer hard-codes transmission to zero —
so a crystal would hide the dial in every image and the caseback window would
hide the movement. They are modelled, and hidden for the renders.

## Files

- `generators/watch/watch_spec.py` — every dimension, CLASS / MOVEMENT / CASE
  as the drone's spec does, and a `declare(app)` that binds them.
- `generators/watch/watch_gearing.py` — cycloidal tooth outlines as sketch
  entities, tooth 0 along +X as `gear.py`'s are.
- `generators/watch/watch_parts.py` — one function per part; a wheel-and-pinion
  helper putting two tooth sets at two depths on one staff.
- `generators/watch/watch.py` — parts, assembly, mates, relations, declared
  clearances, verification, renders.
- `tests/watch_smoke.py` — the parts and the train's ratios, headless.
- `docs/modeling/watch.md` — what building it found.

## Verification

- The layout script's checks become assertions in `watch_spec.py`: no wheel
  crosses a foreign arbor or another wheel's level, every mesh keeps a contact
  ratio above 1.1, and no tip reaches its mate's staff.
- Every mesh, exact, through one pitch of its own smaller gear — the
  whole-assembly motion study cannot see the slow meshes' interference, since
  in one escape pitch the barrel's mesh moves a ten-thousandth of its own.
- `check_geometry` on every part, gated on the receipt.
- An exact interference sweep at rest, gated on the receipt — not on
  `clash: false`.
- `solve_assembly`: dof 0 in the loops, and `placed_by_transform` covering
  only the case parts that are placed, not mated.
- One motion study turning the fourth wheel through 360°: read back the
  poses and require seconds hand +360°, minute hand +6°, hour hand +0.5°,
  escape wheel +12 turns, and no contact that was not there at rest.
- The declared clearances verified, and a render of each side, looked at.
