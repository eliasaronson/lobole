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

Built: see [ISO 286 J and j](../tolerances/README.md#iso-286-limits-and-fits),
[both rings](../parts/README.md#both-rings), the printed
[Hole fit](../modeling/holes-and-threads.md#hole) and
[printed parts](../tolerances/README.md#printed-parts),
`set_tolerance {part}` in [tolerance RPCs](../tolerances/README.md#rpcs), a
round face's [seam as X](../assemblies/solver.md#connector-frames),
[project tables](../variables/README.md#tables), and the tooling in
[scripting](../scripting/README.md).

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

Built: see [feature control frames](../drawings/README.md#feature-control-frames)
on datum features, aligned values and a placed Ø in
[dimensions and annotation](../drawings/README.md#dimensions-and-annotation),
[detail views](../drawings/README.md#detail-views),
[balloon layout](../drawings/README.md#balloon-layout),
[inch screws](../parts/README.md#inch-screws-asme-b183) and material
[grades](../rendering/materials.md#grades); the pan/tilt sheets in
[drawings and renders](../../generators/pantilt/README.md#drawings-and-renders).

## F. The big generators as tests

Built: see [the big generators as
tests](../../generators/README.md#the-big-generators-as-tests).

## Order

A and B first, since they are independent and cost the most. Then C and E.
Then D, after A, and F.
