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

`set_cam_setup` accepts a stock, and the REQUEST behind it — the mode, the
typed size, the placement, the margins — comes back nowhere. The derived box
was already reported, so a fixed size that did not take looked exactly like
one that did. Same class Phase 0 fixed for op fields (`stepover` settable,
unreadable) left open one level up, on the field that decides how much
material you buy.

(First written here as "`stock` is not reported at all", which was wrong: the
box was reported, forty lines further down, by a second assignment that
overwrote the first attempt at this fix. One place now, not two.)

**Fix.** Report the request beside the box it resolves to.

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

## 6. A headless session has no machines at all

`list_machines` returns an empty list over a directory it names correctly and
which plainly contains `6090.json`. `set_cam_setup {machine: "6090"}` is
refused. Every job reports `machine_match: missing` against a machine on disk.

`main()` loads the store with `ui::ReloadMachines(g_App)` — but `RunHeadless`
builds its OWN `ui::AppState`, so the copy the remote server serves was never
loaded. The GUI is fine and `--offscreen` is fine (it drives `g_App`); only
`--headless` is affected, which is the mode an agent and CI actually use.

What it costs: no travel limits, no rapid-feed ceiling, and no post selected
by machine on exactly the surface that cannot see the panel. The waveguide job
could not be told it was running on a 6090 until this was fixed.

**Fix.** `ReloadMachines(state)` in `RunHeadless`, beside the `EnsureParts`
that is already there for the same reason.

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
  a reader would look; this round simply did not read it. (Since fixed:
  `method` is positional-only in both clients, docs/pes-friction-plan.md.)

## Verification

- A refused tool with no number, and an accepted one with it (`headless_smoke`).
- A headless instance lists the machines on disk, and a setup can name one.
- `list_cam_setups` round-trips what `set_cam_setup` took, box included.
- The warning does not fire when `top_z` equals the stock top, and when it does
  fire it prints the difference rather than two numbers that look equal.
- The waveguide's derived stock is ±105 at a 5 mm margin, not ±113.24.
- A unit test on a revolved ARC, which is the shape whose two bounds differ.
  Its first version used a sphere cut from a cylinder and PASSED against the
  bug, because both are analytic and bounded exactly; it now REQUIREs that its
  own fixture is loose (151.30 against 139.78) before asserting anything, and
  fails by 11.5 mm if the conservative bound comes back.
- **No golden moved.** Both `cam-block-*` and `cam-curved-*` are byte-identical,
  which is the right answer: a box and an analytic cylinder have no control
  points outside themselves, so no existing prismatic job's origin shifts. The
  change reaches only parts that were genuinely being oversized.
