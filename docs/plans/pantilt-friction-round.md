# The pan/tilt head's friction round

The pan/tilt head round ranked what cost the user most
(generators/pantilt/README.md, "The friction list"). This round fixes it, in
file-disjoint workstreams, two at a time. Each workstream starts with a short
design note: problem, the shape of the fix, files and verification, in
`docs/<area>/`. It ends with tests that fail before the fix.

## A. Studies as jobs

Built: see [studies as jobs](../simulation/README.md#studies-as-jobs), the
motion side in [assemblies](../assemblies/README.md#motion-studies) and
[what a check costs](../assemblies/README.md#clash-sweeps-what-a-check-costs).
The pan/tilt head's exact pan study (`scratch/pantilt`, study `pan 360`,
measured in a headless instance on this machine): 48 samples, 1294 pair
checks, 144 s before and 54 s after (7 samples: 21.7 s and 6.3 s); the same
inputs run again answer `reused` in under 0.05 s.

## B. CAM honest about tolerance and threads

**Problem.** CAM trusts the model where the model is silent, which risks a
wrong part:

- part tolerances are ignored: the K7 seat is cut at nominal;
- a profile with tabs never verifies complete;
- a Hole has no drill point, and a Thread has no length short of its bore, so
  a blind tapped hole is threaded to the bottom of a flat drill;
- a hole or thread the job does not cut is ignored silently;
- adaptive clearing at its default stepover is slow.

**Fix.**

- **Tolerance bands.** A finish pass aims at the middle of the face's
  tolerance band. The verifier judges the cut against the band, not nominal.
  The posted program states the band it aimed at.
- **Tabs.** Material left only as tabs, or as an offcut cut free, is not
  "leftover".
- **Drill point and thread length.** The Hole feature gains a drill point
  angle (default 118°, derived per the defaults rule) and a thread length
  short of the drilled depth. The ThreadMill and the verifier honour both.
- **Uncut features.** A job's verdict lists every hole and thread of the part
  that no operation cuts, as a finding rather than silence.
- **Adaptive.** Measure where adaptive spends its time on the base plate,
  and fix what the profile shows.

**Files.** `src/core/cam/*`, `src/core/features/HoleFeature.*`,
`ThreadFeature.*`, and the docs in `docs/cam/`.

**Verified by:** the pan/tilt base plate job, re-run:

- the seat aimed into K7;
- the M4s listed as uncut;
- a blind tapped-hole test coupon with a drill point;
- a profile-with-tabs part that verifies complete;
- adaptive timed before and after.

## C. Fits, modelling, tooling

**Fits.**

- ISO 286 J and js classes.
- Both rings of a bearing checked by `check_seat` and `fit_bearing`. The
  inner ring is checked through its seat even though the solid is mated to
  one seat only.
- A Hole's clearance fit applies the part's print allowance.
- `set_tolerance` on any part, not only the active one.

**Modelling.**

- A frame built from a cylindrical face takes the cylinder's seam as X,
  stated in the docs, so a frame on a worm is reproducible.
- The expression language gains a table lookup, for example
  `lookup(table, key, column)` over a project table, so that bearing and
  screw sizes can live in the model.

**Tooling.**

- `headless-preview.sh --stop` finds an instance whatever its
  `XDG_STATE_HOME` was.
- `worktree-build.sh` initialises submodules from main's checkout, without
  the network.

**Verified by:** each item's test, and the pan/tilt `check_fits`/`check_seat`
report with both rings verified.

## D. Topology you can build from

**Problem.** A topology result is a picture and an STL. Re-modelling the
yoke from it took about 4 hours.

**Fix.**

- several load cases in one study, optimised together, weighted by
  compliance;
- keep-out bodies;
- a connectivity filter, so that no floating islands remain;
- a static study run on the optimised mesh before re-modelling;
- a split-for-printing verb that cuts a body into pieces that fit a stated
  bed, by planes, keeping materials and names.

This builds on A's job handle, so it starts after A lands.

**Files.** `src/core/fem/`, the topopt code, `src/femgpu` where the solver
needs it, and the Simulation panel.

**Verified by:**

- the yoke envelope, re-optimised with all three load cases at once and the
  worm blocks as keep-outs;
- the result's static deflection compared with the hand re-model's.

## E. Drawings

- feature control frames on datums;
- rotated dimension text;
- automatic balloon layout and placement of a linear Ø;
- detail views;
- imperial (UNC) screws as standard parts;
- a material grade in the material library (6061-T6, not "ALUMINUM").

**Verified by:** the pan/tilt sheets regenerated, inspected rendered, and
the generator's hand-placed balloons removed.

## F. The big generators as tests

**Problem.** A core change broke the OM615 generator, and nobody saw it for
three days.

**Fix.** A `[generators]` ctest label (tagged `[slow]`) that runs each major
generator's cheapest checking stage against a private headless instance:
om615 `--stage preview`, pes_pump `--no-studies`, chess (a cheap stage) and
pantilt `--from assembly` with saved results reused. Each must finish under a
stated time and end clean. `tools/merge-branch.sh --full` runs it.

**Verified by:** reverting the body-mode fix makes the om615 test fail.

## Order

A and B first, since they are independent and cost the most. Then C and E.
Then D, after A, and F.
