# Mechanism friction: what building a throttle linkage cost

Eight items from the carburettor round (2026-09-05/06). Ordered by what they
cost to work around, not by size. Every measurement here is from that round.

The first one is a defect that produced silently wrong answers for a whole
evening and was misdiagnosed twice before being measured.

---

## U0. Correct the record on `set_mate` — it is a UNIT bug

`docs/known-bugs.md` currently blames the erratic poses on the loop solver
picking an arbitrary branch. That is wrong, and the entry has to be rewritten
before anything is built on it.

`Occurrence.h:152` declares the mate's DOF:

    float rotation = 0.0f; // twist about Z, radians (revolute/cylindrical DOF)

`set_mate` writes its `rotation` parameter straight into that field, and its
`describe` entry is `"Edit a mate's parameters."` — naming `rotation` with no
unit at all. Motion studies convert at the same boundary
(`RemoteMotion.cpp:204`, `* kPi/180` in, `* 180/kPi` out), so the two RPCs
disagree about what an angle is.

Measured, on the carburettor's cable mate:

    set_mate rotation=0.0000  -> bore 2999.6 mm2
    set_mate rotation=6.2832  -> bore 2999.6 mm2     (2*pi: same pose)

A full turn of 2*pi returns the identical pose. The field is radians.

So `rotation=60` pinned the mate to 60 rad = 3437.7 deg = 197.7 deg, and the
"ten-degree walk" 10, 20, 30, 40, 50, 60 was really 213, 66, 279, 132, 345,
198 degrees. That is the entire "random branch" sequence, in order. Nothing
was wrong with the loop solver; the caller was asking for nonsense and the
solver was answering it correctly.

`set_mate`'s `rotation` has no test coverage anywhere in the suite — only
`offset` is exercised (`tests/headless_smoke.py:948`) — which is how a
missing unit conversion survived.

**Fix — DONE 2026-09-06.** Converting `rotation` to degrees was tried first
and reverted: relations speak radians on purpose (`phase` is documented "in
b's unit (rad or mm)", a rack's ratio is mm per radian), so degrees on the
mate alone makes `dof_b = ratio * dof_a + phase` incoherent —
`assembly_relations_smoke` and `motion_smoke` both failed and are what
showed it. The fix is to NAME it, following the convention already in that
file: `rotation_deg` beside `rotation` on `add_mate`/`set_mate`, both at
once refused, both reported. `tests/headless_smoke.py` covers it (the two
spellings are one pose, 360 returns to 0, both-at-once is refused) —
verified to fail against the old behaviour first. known-bugs.md, the
dogfood doc and the generator comment are corrected.

## U1. `contact_report` says which parts and how much, never where

    {"a": "throttle lever", "b": "bellcrank", "volume": 66.26}

Two flat plates, three bosses and a pin each: that is a guessing game, and it
cost four build-and-look cycles on this round alone. The intersection solid
is already computed to get the volume.

**Fix — DONE 2026-09-06.** `Measure::SetContact` carries the intersection's
volume-weighted `centroid` and its bounds, and `contact_report` (part bodies
AND occurrences) plus both `interference` forms report them on every clash.
Absent when the pair merely touches or is clear, so their presence means
"there is solid in solid, and it is here". The motion study's clashes come
from the mesh path and still carry names only.

## U2. `run_motion_study` puts the assembly back where it found it

So nothing about the far end of the travel can be measured afterwards — a
`section` after the run sees the shut throttle twice. The generator had to
read orientations out of the sample poses and do quaternion arithmetic in
Python to recover the angle the butterfly turned.

**Fix — DONE 2026-09-06.** `leave_at` on `run_motion_study`: `"start"`
(the default, unchanged), `"last"`, or a sample index. It scrubs the live
assembly through the same walk the scrub uses, so it arrives on the branch
the run found rather than on whichever one a jump would land on.

## U3. There is no true world bounding box for an occurrence

`get_state` gives each body's bbox in its PART's space. On an assembly tab
there is nothing that gives an occurrence's extent where it actually sits, so
the round rolled its own by transforming the eight corners of the local box —
which is a conservative box, not the part: it claimed the throttle lever
spanned z 8..40 when the real part came near neither end, and two diagnoses
were made from it.

**Fix — DONE 2026-09-06.** `list_occurrences {bounds: true}` adds each
occurrence's world `bbox_min`/`bbox_max`, and `Occurrence.bbox()` wraps it.
Opt-in, because it is a bbox per occurrence and a 197-body import is a
listing people do casually. A placed SUB-ASSEMBLY has no part document to
measure, so its bounds are the union of its flattened leaves — the first
version returned nothing for one, which a caller reads as "no geometry"
rather than "not implemented for this kind".

## U4. `frame` means CREATE on a part and LOCATE on an occurrence

`Occurrence.frame("pin")` returns a named frame's position and axes in world
space, and would have answered every "which way does this grow, where does
that pin land" question in this round directly. It was never found. What was
used instead was the hand-rolled box of U3.

The reason it was not found is that the same verb does the opposite thing one
class away: `p.frame("pin", origin=..., z=...)` CREATES the frame, and an
agent that has just written that twenty times does not guess that
`occ.frame("pin")` asks where it went. `sdk_smoke`'s anti-shadowing rule does
not catch it because the two live on different classes.

**Fix — DONE 2026-09-06.** Both docstrings now name the other, and
`Part.frame`'s says outright that the read form exists one class away and
that a whole round answered its questions by transforming bbox corners
instead.

## U5. Nothing can say a contact is INTENDED

A screw entering the mouth of its tapped hole and a crank running on its
pivot boss are two solids touching. With no way to say so, this round wrote a
substring allow-list — which asked whether NEITHER name in a pair appeared in
the message, and since every pair named the body, it hid all six real
clashes, including a coupler running through both cranks it joins.

Auto-skipping pairs joined by a mate is NOT the fix: the coupler and the
lever are joined by a mate, and that clash was the bug.

**Fix — DONE 2026-09-06.** `check(allow={("a", "b"): max_mm3})`: exact
pairs, each with a volume ceiling, going over is still a failure, and an
allowance that matched no clash is itself an error so the list cannot rot.
Nothing in the engine needs one — the model was fixed instead (U8) — which
is the right outcome for an escape hatch.

## U6. A stale build tree looks exactly like a regression

Twice in this round a commit from a parallel session changed C++ underneath a
build tree, and the suite failed in a way that read as the round's own work:
first the frozen CAM golden, then three version/golden gates. Each cost a
diagnosis detour before the answer turned out to be `ninja`.

**Fix — DONE 2026-09-06.** `test_conventions.cpp` compares the test
binary's mtime against the newest `.cpp`/`.h` under `src/` and `tests/`,
and fails naming the file and the rebuild command. Verified by touching a
source file. It covers the TEST binary only: a long-running `--offscreen`
instance can still be a deleted binary, which happened once more during
this very round and cost another ten minutes.

## U7. `list_parts` returns assemblies too

It carries `type: "part" | "assembly"`, but the name does not hint at it and
the description does not say. The round's folder sweep filed the engine and
the carburettor — the two things in the project that are neither parts nor
fasteners — under Fasteners.

**Fix — DONE 2026-09-06.** `describe` now says it lists every TAB with a
`type`, and names the folder-sweep bug as the reason. `App.parts()` and
`App.assemblies()` filter; the generator uses them.

## U8. The engine assembly runs no top-level clash check

Which is why these have always been there, and nothing said:

| pair | volume |
|---|---|
| head x carburettor | 5795.7 mm3 |
| head x exhaust | 3628.1 mm3 |
| barrel x head | 1145.0 mm3 |
| head x each M10 bolt | 864.9 mm3 |

Spigots and shanks modelled inside solid castings — the same class of defect
the carburettor round fixed, one level up. The motion study cannot see them
because it skips pairs that are rigid to each other, by design.

**Fix — DONE 2026-09-06, and it was not what the table suggested.** The
manifolds were not spigots standing in undersized bores: the port FACES sat
at |y| = 68 while the head's own deck flange carries on to 78, so anything
bolted to a port was mated 10 mm inside solid aluminium. The ports now
stand on one long boss from the wall out past the flange, and sit high
enough that the boss clears the cylinder barrel below it. The bolt heads
were inside the cooling FINS (bolt circle 65, fins 72) and now have an
access counterbore, which is what a real head notches for the spanner. The
carburettor then reached the timing drive, so the drive moved out 25 mm;
and its idle-stop boss dipped 2.5 mm behind its own mounting face into the
port. Eight clashes and ~15,000 mm3 down to none, and `rig.check()` runs at
the end of the build so it stays that way.

Worth recording that the clash CENTROIDS from U1 are what made this an
afternoon rather than a week: every one of the eight named its own location
on the first report.
