# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---


## Adding or removing a CAM operation freezes the app for about a second

Reported 2026-08-26 by the owner, from the panel: adding an operation and
deleting one both stall the window for on the order of a second. The work is
synchronous inside the ImGui frame, so the freeze is the whole app, not the
panel.

**What is measured, on the saved 4-op baffle job** (`scratch/w170s-alto`,
360 x 250 mm stock, drill + two pockets + profile), over the RPC on this
machine:

| step | cost |
|---|---|
| `cam_toolpath` — regenerate EVERY operation (`cam::GenerateAll`) | ~32 ms |
| `cam_verify` — post + simulate + compare (0.25 mm grid) | ~390 ms |
| the same verify at the panel's own 0.4 mm grid | ~150 ms (scaled) |

So on THAT job the two paths do not add up to a second, and the entry says so
rather than declaring the obvious mechanism guilty. What is certain is the
shape: every mutation calls `RefreshCamToolpaths` (CamPanel.cpp), which throws
away all toolpaths and regenerates the whole setup, and the verb paths
(CamTool.cpp) follow it with `RunCamVerification`, which posts the job, parses
the G-code back, re-meshes every body at 0.05 deflection and simulates the
stock cell by cell. Both run on the UI thread between two frames.

**Suspects for the missing order of magnitude**, in the order worth checking:
a bigger part (verify scales with stock area / grid², and a 700 x 400 panel at
0.4 mm is ~1.75M cells); an **adaptive** clearing operation, whose generator
is libarea and is the slow one by a wide margin; more operations than four,
since every one of them regenerates on every edit; and the autosave that a
`projectMetaDirty` edit arms on a 30-part project.

**Found while looking, unmeasured but per-FRAME:** `AppendCamVisualisation`
(UI.cpp, every frame) calls `cam::PickedFaceRegion(cam::BodiesInSetupFrame(…))`
for the hovered face whenever a CAM pick is armed — an OCC transform of every
body plus a section, per frame, while the cursor moves over the model. The
ghost of the selected operation is rebuilt per frame the same way.

**Impact:** the CAM panel is the one panel where a person adds and deletes
several operations in a row, so this is a second per step through exactly the
loop the tool exists for.

**Likely fix**, in the order that pays: regenerate only the operation that
changed and keep the others (the toolpaths are already per-operation); cache
`BodiesInSetupFrame` and the model height map per document version instead of
rebuilding them per call and per frame; then move regenerate+verify off the
frame — a worker with a "checking…" state, the way the study solves already
work. **Pin it first**: run `--perf-hud` (worst-frame attribution) and do the
add/delete, which is how the subassembly-hide freeze below got its number.

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

## Hiding a subassembly costs ~300 ms — a visibility toggle runs a full renderer rebuild

Found 2026-08-26 by the perf HUD's worst-frame readout, on its first day in
the field: `worst 315.29 ms · update 301.41` the moment a subassembly was
hidden on the dock (76 occurrences).

**Root cause (known):** toggling occurrence visibility bumps the assembly
version, and `SyncDocumentRenderers` answers any version change the same way
— remove and re-add every mesh, recompute section caps. The renderers already
have the cheap path (`SetInstanceVisible`); nothing routes a visibility-only
change onto it.

**Impact:** a one-off ~300 ms freeze per hide/show on a medium assembly,
scaling with occurrence count. Not per-frame. Hide/show is a first-hour user
action on any assembly, so this is worth fixing before strangers arrive.

**Likely fix:** an incremental path — a visibility-only change flips
`SetInstanceVisible` on the affected occurrence subtree and skips the
rebuild. Deliberately NOT done in the same round that found it: sync
granularity is exactly where staleness bugs live (the
raw-block-instead-of-result class), so it wants its own session with tests
that sabotage the "did the ghost/section/highlight state survive" corners.

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
