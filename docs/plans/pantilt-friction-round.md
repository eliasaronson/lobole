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

Built: see [tolerance bands](../cam/operations.md#tolerance-bands) and
[their check](../cam/simulation.md#tolerance-bands),
[tabs and offcuts](../cam/simulation.md#tabs-and-offcuts),
[a drill's point](../cam/operations.md#drill) and
[a Hole's point and thread depth](../modeling/holes-and-threads.md#hole),
[what the job does not cut](../cam/simulation.md#what-the-job-does-not-cut),
and libarea patch 0002 (`deps/libarea/README.md`). The pan/tilt base plate
re-run: the K7 seat aimed at 31.9945 and cut 31.9946 by the check, in
31.982-32.007; the four M4 threads listed as not cut; both setups ok and
complete at 0.1 mm. Adaptive on its Top setup: the same 78036 moves in
26-28 s against 34-40 s, A/B on one build.

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
