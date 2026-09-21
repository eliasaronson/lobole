# Friction after the helical round

Five fixes the rotary rounds ran into. The first two are checks that report
something they do not actually know; the other three cost time on every run.

## 1. A rotary verdict says WHERE its leftover is

`CompareRotary` fills in `leftoverArea` and never `leftoverRegions` or
`biggest`, so every rotary `cam_verify` answers "0 regions" next to hundreds
of mm² of leftover. And its one worst-cell number is the end dome on every
rotary job, because no rotary pass reaches a face turned toward the axis's
ends. That is why the helical ridges (0.69 mm) took a throwaway diagnostic to
find.

- **Regions:** the cells over the leftover floor, joined across neighbours
  (wrapping round θ), sized by arc at their radius. `leftoverRegions` and the
  biggest few, the same shape the height-map check reports.
- **Surface against end faces:** a designed cell is END-FACING when its
  surface climbs faster than 70° along the axis (|∂r/∂x| > tan 70°), which is
  where a rotary pass cannot reach. The verdict gains `leftoverSurfaceMax` /
  `leftoverSurfaceAt` and `leftoverSurfaceP99` over the other cells, and each
  region says whether it is on an end face. `cam_verify` reports them for
  rotary jobs; the panel's sentence names the end faces instead of calling a
  dome "leftover".

**Verified by:** a test shaft whose end faces are the only leftover reports a
surface maximum near zero and every region as end-facing, and a deliberately
skipped band reports as a surface region. Re-checking the pin jobs reproduces
the diagnostic's numbers (surface worst about 40 µm for parallel, 61 µm for
helical).

## 2. The params scanner follows every helper

47 `// rpc-params:` annotations stand in for helpers the scanner cannot
follow, and a stale one is what hid the SDK bug. `tools/rpc_params_scan.py`
generalises `KNOWN_HELPERS`: any function in the Remote*.cpp files that takes
an `RpcParams&` is scanned, recursively, and its reads count for every branch
that hands it the params. Annotations the scanner now covers are deleted. The
rest stay, and each of those is a real opaque case or a preamble key.

**Verified by:** the params gate on the full RPC surface. Any mismatch it now
finds is a real bug, fixed in its row.

## 3. Continuous programs stop restating F on every line

A scaled F differs by a tenth from line to line, so the rolling pin's spiral
is 35 MB, mostly F words. The post keeps the F already in force when the new
one is within 1% of it. No move runs more than 1% off its scaled feed, and it
never goes over an axis limit, because the F in force was itself within them.

**Verified by:** the spiral file before and after (size, and the verifier's
stock bit for bit the same, since F moves nothing). Cycle time from the file
agrees with `CuttingMinutes` to 1%, and the feed tests' tolerances say so.

## 4. The generators can run twice in one instance

`rolling_pin_job.py` (and the handle and waveguide jobs) refuse to build into
an instance that already has their tab, which is right, and meant restarting a
scratch instance for every comparison. `--fresh` starts a new project first.

## 5. A simulation screenshot shows the part

After `cam_simulate`, an off-screen screenshot was zoomed into the stock,
because nothing had framed the camera since startup. Opening a simulation over
the RPC frames the job, once: the camera is left alone on a later seek, so a
caller who set a view keeps it.

## As built

**1.** As planned. Its first real answer was not the one the plan expected.
Re-checking the saved pin jobs, the surface's 99th percentile matches the
throwaway diagnostic (31 µm parallel, 30 µm helical). But the surface MAXIMUM
is 1.25 mm for both, at X0.6: the foot of the pin's domed end, just under the
70° end-face line. The ball cannot get down it because the rotary generator's
grid stops at the part's end, and reaching the steep part of a dome needs the
tool centre beyond x = 0. So the measure is right to call it surface:
reachable, and not reached. Letting passes run a tool radius past each end
would finish the domes. It changes every rotary toolpath and takes the cutter
further toward whatever holds the work, so it is left as a decision
(docs/cam/dogfood-rotary.md).

**2.** 22 of the annotations went. The 25 that remain are all reads the
scanner cannot see: 16 `job` keys read before the CAM dispatcher's branches, 6
`ItemTarget` constructors, and 3 sets of keys passed to a lambda as arguments.
The gate stays green on the whole RPC surface.

**3.** Smaller than claimed. F words fell 17% (829k to 688k), and the spiral
file 3% (35.2 to 34.1 MB). The premise was wrong: F words are about a fifth
of the bytes, and a scaled F usually moves more than 1% between moves because
the radius changes across the flutes. The spiral is big because it has 837k
lines, and that comes from its spacing densifying on every flute wall. The
change stays, since it is correct and removes 140k redundant words.

**4.** `--fresh` on `rolling_pin_job.py` and `handle_job.py`, through
`fresh_project` in `generators/_lib.py`, which refuses when the open project
is dirty. Both generators' refusals now name the flag. The waveguide job
never had the guard.

**5.** `cam_simulate` frames the job once as it opens a playback, keeping the
camera's direction. A pin simulated and screenshotted with no view calls comes
out framed.
