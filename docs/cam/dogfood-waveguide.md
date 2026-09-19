# Waveguide dogfood — the friction it found, and what to do about it

`generators/waveguide.py` is a real part cut on a real machine's terms: a
200 mm waveguide for the Alpair 7MS, bolting to the smart_speaker baffle on
the driver's own bolt circle. It was built to exercise 3-axis surfacing
(docs/cam/surfacing.md) and it did: **max gouge 20 µm** through the real post
and the real verifier, inside the 34 µm scallop a 6 mm ball leaves at that
stepover, on a part with two curvature scales an order apart.

What follows is everything that got in the way while driving it. Four are
defects; two things that looked like defects are not, and saying so is part
of the record.

## 1. A tool with no number silently edits tool 1

`RemoteCam.cpp:1234` reads the tool's number as `tj.value("number", 1)`. A
call that does not say which tool it means therefore EDITS TOOL 1 — renames
it, resizes it, changes its type — and reports success.

That is how this round lost twenty minutes: the row reads

> or one of its tools `{id, tool: {number, name?, …}}`

which parses naturally as a nested object, so the call went in as
`tool={"id": sid, "tool": {...}}`. `number` was then absent, defaulted to 1,
and the 6 mm ball intended as tool 2 quietly became a rename of the 6 mm
flat. No error, and `list_cam_setups` showed one tool where two were asked
for.

**Fix.** A tool object with no `number` is REFUSED, naming the field — this is
the project's own rule and the default is a substitution of exactly the kind
it forbids. And the row says `{id, tool: {...}}` are SIBLING parameters,
because the sentence that misled a caller once will mislead the next.

## 2. The stock is settable and unreadable

`list_cam_setups` reports seventeen keys and `stock` is not among them, while
`set_cam_setup` accepts one. So a script can state the blank it is cutting
from and cannot read back what the setup thinks it is — and when the two
disagree, nothing says so. This is the same class Phase 0 fixed for op fields
(`stepover` settable, unreadable) left open one level up, on the field that
decides how much material you buy.

**Fix.** Report `stock` — mode, size, place, margins — and the derived box it
resolves to, which is the number a caller actually wants.

## 3. "top_z 25 is below the stock top (Z25)"

Both numbers print as 25 and the sentence contradicts itself. Neither half is
a formatting slip:

- the guard is `op.topZ < stockHi.z - 1e-6`, a tolerance of one NANOMETRE
  against a bound derived from float32 geometry, whose error here is
  1.9 nm — so a `top_z` typed to exactly the stock top trips it;
- the message formats both with `%.4g`, so the difference it is complaining
  about cannot appear.

**Fix.** A physical tolerance — 1 µm, still a thousand times finer than any
router cares about — and a message that cannot read as nonsense.

## 4. The stock box is the CONSERVATIVE bound

`CamSetup.cpp:81` calls `GetBoundingBox(min, max)` with the default, which OCC
computes from a B-spline's CONTROL POINTS. For the waveguide, whose surface is
a revolve, that reports ±108.24 mm on a part that is ±100.00 — so the blank is
16 mm oversize across, and the facing and profile passes cut air to reach it.

`CADKernel.h` already says which answer is which: the conservative one is for
"a broad phase, a scene-reach or a does-this-pair-need-checking", and `tight`
"is what a PERSON or a script wants, because a part whose reported size is
sixty per cent over its real one reads as a modelling error". Stock is the
size of material a person buys and clamps. It wants the tight one.

**This one changes output.** The stock box sets the work origin, and the post
subtracts the origin, so every coordinate in every posted file for a job with
curved geometry moves. `tests/golden/cam-curved-*.nc` exists precisely to make
that visible, and it must be regenerated deliberately with the diff read.
Anyone holding a program posted before this change re-zeroes against the new
origin or cuts the part in the wrong place.

## 5. The V-bit refusal still names three types

`CamDerive.cpp:126` refuses a cone for `Pocket || Face || Adaptive` by
enumeration, which Phase 0 left behind and phase 1 noted. Surfacing has its
own refusal now, so the enumeration is the last op-type predicate in the CAM
core that the capability table does not answer.

**Fix.** A `NeedsFlatFloor` column, and the predicate asks it.

## Two that are NOT defects

- **Roughing a dish works.** The finishing verbs say to rough first, and it
  looked as though nothing could: `GenerateAdaptive` with no target falls back
  to sectioning for HOLES, and a dome has none. But a dish DOES section into an
  annulus whose inner wire is a hole, so an adaptive op with no pick roughs
  the waveguide in 18,345 moves. The worry was unfounded.
- **`d.call("help", method=…)` is a documented trap, not a bug.**
  `designer_remote.py:40-45` says `help` and `describe` take a parameter called
  `method`, which collides with `call`'s own argument, and that those two go
  through `request()` instead. The escape hatch exists and is explained where
  a reader would look; this round simply did not read it.

## Verification

- A refused tool with no number, and an accepted one with it (`headless_smoke`).
- `list_cam_setups` reports a stock that round-trips what `set_cam_setup` took.
- The warning does not fire when `top_z` equals the stock top, and when it does
  fire the two numbers it prints differ.
- The waveguide's derived stock is ±100, not ±108.24.
- `tests/golden/cam-curved-*.nc` regenerated, diff read, and the block goldens
  UNCHANGED — a prismatic part has no control points outside itself, so its
  stock and origin must not move.
