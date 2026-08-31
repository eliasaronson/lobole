# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---


## The viewport tears on the external monitor (environment, not the app)

Reported 2026-08-31 while reviewing the 30-degree lens, and worth recording
because the obvious suspect — the swapchain — is the wrong one. Outside
`--gui-tests` the app requests `VK_PRESENT_MODE_FIFO_KHR` and nothing else
(main.cpp:884; `g_DisableVSync = args.runGuiTests`), which is vsync.

**Root cause is the path AFTER the app hands the frame over.** This machine
renders Designer on the discrete GPU (`Selected GPU: NVIDIA GeForce RTX 4060
Laptop GPU`, the deliberate preference — RT needs it) while X drives the
display through `modesetting` on the Intel Arc iGPU. So every frame crosses
GPUs by PRIME offload and is then blitted to scanout by X, with no compositor
running (i3 on bare X11) and no `TearFree`. FIFO governs the first handoff;
nothing syncs the last one. `nvidia_drm` is not even loaded, so there is no
PRIME sync either.

**Impact:** a tear line while orbiting, on the EXTERNAL monitor only — with
two outputs and no compositor, X syncs to one CRTC and the other tears
whatever it does. The internal panel also sits at 60 Hz while its mode list
offers 165, which makes the artifact three times longer-lived than it needs
to be. The 30-degree lens did not cause this: it made it more visible, by
filling more of the screen and so putting the part's edges — where a tear
line is easiest to see — further out.

**Fix is environment-side, and the user has chosen to leave it:**
`Option "TearFree" "true"` on the modesetting device section (NixOS:
`services.xserver.deviceSection`), or picom with `vSync`, or eDP-1 at 165 Hz,
or Preferences > Graphics device > Intel Arc to drop the cross-GPU hop (costs
the 4060's speed and ray tracing — a diagnostic, not a home). Nothing to
change in the renderer; do not go looking at present modes again.


## Generating an adaptive pocket takes ~7 s on the UI thread

Measured 2026-08-26 on the saved baffle job (`scratch/w170s-alto`, a
300 x 190 mm pocket, 6 mm cutter): the same pocket cut concentrically takes
**30 ms** and cut adaptively takes **7 127 ms** — 26 253 moves out of
libarea. It runs inside the frame, so the app is frozen for those seven
seconds with nothing on screen to say why.

This is the residue of the add/delete freeze fixed the same day (the panel
no longer regenerates the whole job on every edit — one operation at a time,
`cam/incremental_toolpaths_never_go_stale`). What is left is honest cost:
the FIRST generation of an adaptive operation, and any edit to that
operation, still pay it in one blocking lump. A pocket deep enough to want
adaptive clearing is exactly the pocket a person adds and then adjusts.

**Impact:** seven seconds of frozen window per adaptive-pocket edit, on a
job of one operation. Nothing is lost and the result is right — it is the
one big cost in CAM that is not incremental.

**Likely fix:** generate off the frame. The verb already knows the answer
takes a while, so the shape is a worker plus a "cutting…" state on that
operation's row — the pattern the FEM studies already use — with the
backplot showing the previous toolpath until the new one lands. Cheaper
half-measures first if the worker is too much: cache by operation
parameters + document version so a re-open or an unrelated edit never
re-runs libarea, and time the verification pass (207 ms at the panel's
0.4 mm grid) while there.

---

## mesh_study/run_study are synchronous past the client's default timeout

Found 2026-08-26: a modal study on a baffle with six Ø3 pilot holes meshed
to 41k tets; the blocking `run_study` outlived designer_remote's 60 s
default socket timeout, the client died mid-call, and the server finished
anyway — the results were there on reconnect, which LOOKS like a hang
followed by a mystery. `run_study` has `async` + `study_status` polling,
but nothing steers a caller there, and `mesh_study` has no async at all.
**Partially fixed 2026-08-26** (friction round): `designer_remote.request`
takes a per-call timeout and mesh_study's describe doc names the trap.
Remaining: an async path for `mesh_study` itself, and/or a server-side
hint when a synchronous solve exceeds ~10 s.

---

## An assembly resync re-tessellates every part it draws — FIXED 2026-08-30

Was: "hiding a subassembly costs ~300 ms". Measured properly 2026-08-26 on
the Marsliden dock (98 instances, 17 distinct parts), by timing the phases of
the rebuild a visibility toggle runs:

| phase | before | after |
|---|---|---|
| outline `AddMesh` x98 (upload + adjacency, per instance) | 394 ms | ~0 (one asset per part) |
| outline `RemoveMesh` x98 (a device wait EACH) | 81-191 ms | ~5 ms |
| mesh `RemoveInstance` (a device wait per asset destroyed) | 18-56 ms | ~2 ms |
| **tessellation, 17 parts (OCC mesh + edge polylines)** | **177 ms** | **177 ms — still there** |
| `AddInstance` x98 | 0.0 ms | 0.0 ms |
| face selector x98 | 2.4 ms | 2.4 ms |

The two renderer costs are fixed (`assembly/outlines_are_instanced_per_part`
gates the sharing and its refcount): a toggle round trip on the dock went
1395-1608 ms to 701-804 ms, measured the same way.

**FIXED 2026-08-30** (`c3c5169`, `3b5391b`), by the cache this entry
predicted. `AppState::partRenderCache` keys the per-part tessellation and both
renderer assets on (part slot id, part document version);
`MeshRenderer::RetainAsset` / `OutlineRenderer::RetainAsset` are the reference
the lifetime needed, so the assets survive the instance teardown at the head of
every resync.

The real numbers came from a much bigger part than the dock — a 299-part,
314-occurrence STEP import, where the same defect cost 5.9 s rather than
177 ms:

| | before | after |
|---|---|---|
| resync (tab switch) | 5900 ms | **30 ms** |
| visibility toggle | 5890 ms | **39 ms** |
| two assemblies alternating | 4157 ms | **41 ms** |

Two things the fix taught that the plan above did not anticipate:

- **The eviction rule matters as much as the cache.** A strict "release
  anything not drawn this sync" made two assembly tabs THRASH — alternating
  between two 299-part assemblies re-tessellated all 299 every switch. It is
  an LRU against a device-memory budget now
  (`kPartRenderCacheBudgetBytes`), never evicting what the current sync draws.
- **The palette was order-dependent.** The fallback body colour was a counter
  over encounter order, so hiding one occurrence recoloured unrelated parts —
  invisible until a cached colour disagreed with a freshly built one. Seeded
  from the part now.

**Still not doing:** the "visibility-only fast path" this entry used to
propose (route a hide onto `SetInstanceVisible` and skip the rebuild). At
39 ms the remaining win is nil, and it would put a second, differently-shaped
code path through the sync — which is where staleness bugs live.

**Still there:** a COLD build (first view of an import) is ~6.7 s, of which
~4 s is OCC tessellating 299 parts serially. See the follow-ups in
docs/interchange/import-plan.md.

## `fem_async_smoke` fails under machine saturation (twice on 2026-08-26)

Both failures happened while the GUI shards and `ctest -j$(nproc)` ran
CONCURRENTLY (one invocation launched both); it passes immediately alone and
in normally-loaded full runs. Async solve polled against a wall clock + a
saturated machine is the familiar shape — but unlike the old fem_smoke
story, the L8 launcher now keeps the app log, so the next failure is
diagnosable instead of folklore. Not yet earned the "flaky" label; rerun,
and read the log if it happens OUTSIDE a double-suite invocation.

---

The order-dependent GUI failure recorded here on 2026-08-25 —
`terminal/off_by_default_docks_right_on` failing behind
`cam/tool_window_follows_the_viewport` — **does not reproduce and the entry
was withdrawn the same day.** Verified three ways: the full GUI suite passes
**211/211 under `--serial`** (the check the original note said was the real
one), the documented two-test reproduction passes, and the interaction is
still genuinely there — the CAM test continues to toggle the terminal
preference and rebuild the dock layout twice.

**The fixing commit was not identified**, and that is worth saying rather than
implying. `b1f0330`'s dock-flag change was the obvious candidate and is not it
(reverting that hunk on top of HEAD does not bring the failure back), and a
bisect back to 2026-08-21 found the pair passing at every commit sampled. So
it was fixed somewhere in the 08-22..08-24 window by something that did not
announce itself.

What that leaves behind, in [loose-ends-2026-08-25.md](loose-ends-2026-08-25.md)
L3: the behaviour is now NAMED by
`terminal/survives_a_dock_layout_rebuild` instead of being an accident of a
CAM test, so a return fails for the right reason — though that test is a
characterization test, not a verified regression test, because no sabotage
reproduces the original.

## The GUI suite has order-dependent failures nobody has caught in the act

**Three different tests did this on 2026-08-25 alone**, each failing once in a
sharded run and passing on the next run of the same binary:

| test | how it failed |
|---|---|
| `terminal/off_by_default_docks_right_on` | the panel docked but `Hidden` (diagnosed, then stopped reproducing — see below) |
| `conflicts/resolve_a_merge_in_the_panel` | `panel != nullptr` — the Conflicts panel not found |
| `compare/compare_shape_against_a_version` | in the final verification run of this round |

One is an incident. Three in a day is a property of the suite. The shape is
always the same — a test that fails only in company, where the 4-shard
partition decides who its company is — and machine load is a plausible
accelerant: the run that failed shared the machine with a full headless suite,
and the clean rerun of the same binary took 610 s against 205 s.

The first (`terminal/off_by_default_docks_right_on`) was diagnosed to a window
left `Hidden` after a dock-layout rebuild, went away without anyone
identifying the fix, and is withdrawn below. This one is not diagnosed at all.

**Hunted 2026-08-25, and not caught.** Five shuffled runs — three idle, two
under a concurrent headless suite, seeds recorded — all passed 215/215. The
class did not reproduce on demand, so it stays open and undiagnosed. What the
hunt DID find is below; it is a real defect of the same family (a shared
global that makes concurrent shards affect each other) but **there is no
evidence it caused these failures**, and saying otherwise would be the
plausible-and-wrong answer this file exists to avoid.

> **Found and fixed while hunting:** the terminal panel opened its remote
> interface by walking a FIXED range, 8437..8456, one bind at a time — making
> a 20-port window a resource shared by every process on the machine.
> Measured with four concurrent GUI shards: they collided, walked to 8438 and
> 8440, and logged an `error` per failed bind — up to 19 socket/bind/close
> syscalls **on the UI thread, mid-frame**, in a suite whose tests wait fixed
> frame counts. It now tries the documented default 8437 once (so a client
> configured for it still finds a lone instance) and otherwise asks the
> kernel, which is one syscall and cannot collide. Same class as the
> `free_port()` race in the smoke scripts, one layer up.

**What would actually help**, in order: `--gui-test-shuffle` exists and CI runs
it (`d1396b5`), so the cheap move is running the suite shuffled a few times
and **keeping the seed of a failure** — failures are reproducible per seed,
which is the whole point of the flag. Chasing one from a single observation is
what the terminal case shows does not pay: it was diagnosed to a window left
`Hidden` after a dock-layout rebuild, four fixes were tried and rejected, and
then it stopped reproducing without anyone identifying what fixed it.

~~A second, cheaper move given the load correlation: capture the shard logs on
failure rather than leaving them in `/tmp` to be overwritten by the next
run.~~ **Done 2026-08-25.** A failing shard is archived under
`$XDG_STATE_HOME/designer/gui-failures/<stamp>-shard<N>/` — which no later run
touches — with its full log, its exact filter, and a runnable `reproduce.sh`
carrying that filter and the seed. The fixed `/tmp` copies are still cleared
each run, so nothing stale can be read as current; the two behaviours were in
tension and the archive resolves it.

The tension is worth stating, because it is why three failures in a row went
undiagnosed: **the natural response to an order-dependent failure is to
re-run it, and the re-run destroyed the evidence.** The next one will leave a
directory behind that names which tests shared its process and in what order
— the two facts that make this class reproducible at all.

---

The orbit-lag + stale-silhouette pair in the outline renderer was fixed by
the [rendering/outline-perf.md](rendering/outline-perf.md) work (precomputed
edge adjacency, per-frame-in-flight edge buffers, settle-pass policy),
covered by `tests/test_outline_edges.cpp`.

The preview-handle miss regression was fixed in `PreviewHandles.cpp` and is
covered by the `fillet_body_edge_via_click` GUI test.

---

## `item_click` does not leave a text field focused (ui_action, 2026-08-27)

Every remote UI action is queued, run and unwound before the RPC returns, so
the widget it touched is not active by the time the next call arrives. On a
button that is irrelevant; on an `InputText` it means **no remote script can
drive a multi-step text interaction** — click the field, then send keys, and
the keys go nowhere. Verified by clicking a dimension's expression field and
sending `Left`, `Delete`, `Enter`: the text was unchanged.

`item_input` sidesteps it by doing the whole thing in one action (click, type,
Enter), which is why every existing script and test uses it and nobody noticed.

Two consequences worth knowing before reaching for the remote:

- A **transient popup cannot be screenshotted** — anything that only exists
  while a field is focused (the u14 variable-completion popup, an in-progress
  tool prompt) is gone by the time `screenshot` runs. This was found trying to
  photograph exactly that.
- A field cannot be left mid-edit for a human to look at.

The fix would be either an action that leaves focus in place, or a `key_chars`
action to pair with `item_click`. Neither is needed by anything today, which is
why this is filed rather than fixed.


## CAM: two defects found machining the smart_speaker front_wall (2026-08-31)

Both found by the verifier on a real 2-sided job, both reproducible, neither
fixed yet. `tools/` has the job script that reproduces them:
`front_wall - Front.ngc` / `- Back.ngc`.

### A rapid plunged to the previous depth at the RAMP ENTRY (FIXED 2026-08-31)

Between depth passes a clearing operation retracted to safe Z, repositioned,
and then `G0` straight down to the height it was about to start cutting at —
on the reasoning, written at the ramp entry, that that height "is either the
stock top or the depth the previous pass already cut here, so it is air".

True of a closed contour, which returns to its own entry at full depth. False
of a POCKET, whose entry sits mid-spiral where the previous pass was still
ramping, and false of ANY pass told to start below the stock top — a finishing
skim after a roughing pass is exactly that. On the front_wall job it was 3 mm
of G0 into MDF; in the golden program it was `G0 Z12` with the stock top at
21.

Fixed by `RapidFloor`: a rapid never goes below the STOCK TOP, above which
there can be no material. Everything under that is left to the ramp, whose
moves are feeds — which also keeps the rule a profile depends on, that it
never plunges (a non-centre-cutting end mill rubs). The golden programs show
the change as `G0 Z12` becoming a fed `G1 ... Z11.92 F200`.

Gated by `cam: a pass never rapids to a depth nothing cleared`, which checks
`rapids_through_stock == 0` for both shapes: a pocket deep enough for two
passes, and a skim starting below the stock top.

### A profile cut ONE contour at every depth — FIXED 2026-08-31

Front_wall's cross-dowel bores are cut 22 mm deep from the x = ±105 side
plane (`19-sketch.json` + `20-extrude.json`: two Ø8 circles at sketch
(-8, ±461), `distance: 22.0`, `mode: cut`). 22 mm from a face 105 out means
they OPEN ONTO that face — measured, the side face loses 100.53 mm² to them,
which is exactly two Ø8 circles (2 · π · 4² = 100.53).

So at the bores' own height the panel's outline genuinely has two 8 mm slots
running 22 mm in from each edge, and above and below that height it does not.
`PickedWallContour` takes ONE contour and cuts it at every depth — the
behaviour is documented ("a wall names one contour at every depth", which is
right for a through opening that has no floor to click) — so the slot contour
gets cut through the whole 16 mm thickness, removing the 4 mm of material
above the bore and the 4 mm below it.

That is the gouge, and the arithmetic matches: `cam_verify` reports **8.00 mm**,
which is 4 + 4. A targetless "cut out the whole part" reports the same 8 mm,
so it is not the wall pick.

Bisected on the real part, rebuilding the job for each case:

    everything present            gouge 8.00 at (-104.2, -464.6)
    cross-dowels 19/20 out        gouge 8.00 at (-104.2, -464.6)
    cross-dowels 21/22 out        gouge 8.00 at ( 103.8, -464.6)   <- other edge
    ALL FOUR cross-dowels out     gouge 0.00
    chamfer 42 out                gouge 8.00
    blind dowels 17/18 out        gouge 8.00

So: the cross-dowels alone, neither the chamfer nor the blind holes, and not
their intersection. Two earlier diagnoses were wrong and are struck: it is not
the section merging a hole with the boundary (a lone hole 1.7 mm beneath a
face sections cleanly — 2 loops, outer extent unchanged), and it is not the
two holes intersecting (built as a fixture, also clean). The section is right
throughout; what is wrong is using one section for every depth.

**Attempted and reverted, 2026-08-31.** Sectioning per depth is the right
rule — FreeCAD does exactly that, and `Profile.areaOpUseProjection` returns
True where `PocketBase`'s returns False, so a profile is meant to use the
projection down the depths and a pocket is not. `PickedWallContour` gained an
`atZ` and the profile asked for the contour at each pass; measured on the real
part, that DID narrow the damage — the notch is entered at one depth instead
of all four — and the verdict did not move, because a height-map verifier
counts the whole column the tool sweeps to reach that depth. Accumulating the
reachable outline down the passes (union outside, intersection inside) is the
next step and did not work either.

It was reverted rather than left in, for a reason worth recording: **four
attempts at a synthetic reproduction all failed.** A lone blind hole 1.7 mm
beneath a face sections cleanly. Two intersecting holes section cleanly. A
bore modelled to open onto an edge reports `maxX = 210` at every height — no
slot at all. Whatever the real part does, it is not what any of those
fixtures do, and changing generation with no test that fails before and passes
after is how this codebase gets a silent regression. The bisect stands (the
cross-dowels, alone, 8.00 mm = 4 above the bore + 4 below); the mechanism does
not, yet.

### The section was dumped, and the fixtures were never the problem (2026-08-31)

That is what the paragraph above asked for, and the answer is the first of the
two it offered: **the outer loop has the notch.** `tests/test_cam.cpp`,
hidden, `designer_tests "[frontwall]"` (the real part is not in the repo;
`DESIGNER_FRONT_WALL` points it elsewhere).

Sectioning the real front_wall in the Back setup, outer loop, in path order at
setup z = -8:

    [3] 105.000 -465.000    <- on the edge
    [4]  97.000 -465.000
    [5]  83.000 -465.000    <- 22 mm in, the extrude distance
    [6]  83.000 -457.000    <-  8 mm across, the bore diameter
    [7]  97.000 -457.000
    [8] 105.000 -457.000    <- back onto the edge

Four of those, one per cross-dowel. They exist for z in (-12, -4) — the bore's
own extent — and nowhere else: 24 outer vertices at z = -8, 8 at z = -2. The
control settles it, since it is the bisect's second row run as code: with the
job's own configuration 43 (cross-dowels suppressed) the outer loop is 8
points with **zero** vertices anywhere near y = -461. The areas agree to the
square millimetre — 218259.70 without the bores, 217555.70 with them, a
difference of 704.00 = 4 x 22 x 8.

So the section is right, the diagnosis above is right, and the depth
arithmetic closes exactly: the bore spans 8 mm of a 16 mm panel, the profile
cuts its contour over the full thickness, and 16 - 8 = the 8.00 mm `cam_verify`
reports.

**The four fixtures did not fail. The check used to judge them was blind.**
`maxX` cannot see this defect — a slot cut INTO an edge does not move the
bounding box, and the real part reports the same 210 mm width at every height,
notch and all. Rebuilt as its own fixture (a 210 x 100 x 16 panel, one 8 mm
bore 22 mm in from the side face, no setup frame), the synthetic case
reproduces immediately:

    z  -8.000: outer  8 points, area 20824.000, maxX 105.000 (width 210.000)
    z  -2.000: outer  4 points, area 21000.000, maxX 105.000 (width 210.000)
    z -14.000: outer  4 points, area 21000.000, maxX 105.000 (width 210.000)

21000 - 20824 = 176 = 22 x 8, and the width the reverted round measured is
210.000 on all three lines. That fixture is the failing test the revert said
was missing, and it costs no external file.

### The fix: a profile follows the projection, not the section

**Reading the generator moved the diagnosis.** The op that gouges — op 87,
"Cut out", `side: outside` — has NO targets, and the targetless branch of
`GenerateProfile` was *already* sectioning per depth
(`SectionLoops(bodies, z + 1e-3)`, one section per pass). So "one contour at
every depth" described the wall-pick path but not the path that actually
failed, and that is why the reverted change did not move the verdict: it made
the picked path do what the failing path was already doing. **Sectioning at
the right depth was never the fix.**

The rule that is:

> The contour at depth z is the part's PROJECTION from `op.topZ` down to z —
> the union of its sections over that interval — not its section at z.

A flat end mill at depth z removes the whole column above it at its footprint,
so it may only travel where every height in [z, topZ] is waste. Two things
made this smaller than the reverted attempt:

- **It is one rule, not two.** "Union outside, intersection inside" are the
  same statement seen from either side — the sweepable region is the
  complement of the projection — so `ProjectedOutlines` carries its holes and
  an outside cut follows its outer boundaries while an opening follows the
  hole it was picked in. Implementing it twice is how one of them goes wrong.
- **It accumulates from `op.topZ`, not the stock top.** An operation's top is
  the declaration that everything above it is already clear, which is what
  lets an opening that starts below the stock top be profiled at all.

`ProjectedOutlines` (CamToolpath.cpp) walks the passes unioning as it goes.
Below the model it adds nothing and the last projection stands, which is what
a through cut needs and what the old `lastFound` was for — except that
`lastFound` carried the last *section*, so it propagated the notch to every
pass under the part. Three sites changed, all in `GenerateProfile`: the
targetless branch, the wall pick (re-resolved per pass against the projection
by proximity to `WallContour::at`, which the pick now reports), and the
through-cut carry.

**Measured, before and after, on the real front_wall with the cross-dowels
present:**

| | before | after |
|---|---|---|
| Back setup, `cam_verify` gouge | **8.000 mm** at (-104.3, -464.9) | **0.005 mm** at (-55.8, -506.9) |

0.005 is grid quantisation, and it is somewhere else entirely.

Gated by three tests in `test_cam.cpp`, and the second one is the point:

- `cam: a profile never follows a notch it cannot reach` — the fixture above.
  Before: 4 moves inside the notch, gouge 11.0 mm. After: none, gouge < 0.5.
- `cam: a profile DOES follow a notch open from the top` — the same notch
  reached from the top face instead, where nothing stands over it. It passes
  before AND after, which is what stops the rule degenerating into "never cut
  a notch".
- `cam: a picked wall is re-resolved at every depth` — the same defect through
  the wall pick, whose contour came from the wall's own mid-height: for a
  16 mm panel that is z = -8, the one height whose section is notched. Before:
  10 moves in the notch. After: none.

The whole headless suite passes (925/925), CAM goldens included — the change
is a no-op on a prismatic part and bites only where the outline moves with
depth.

**Not changed, and worth naming:** the floor-region pick (`PickedFaceRegion`,
which pockets use) still takes one region at its own height. The same column
argument applies to it in principle; there is no reproduction, and this file's
own history says what changing generation without one costs.

The job may now keep the cross-dowels, though suppressing them remains
reasonable: the machine does not make those holes — they are drilled by hand.

