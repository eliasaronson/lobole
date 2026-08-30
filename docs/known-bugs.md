# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---


## Adding a renderer means ~20 hand-maintained wiring sites

Two renderers have now shipped with a full-window viewport because they were
left out of one list in `UI.cpp` — `0346a8e` (FEM field) and `4a3083d` (CAM
stock). Every omission in that wiring fails SILENTLY: wrong scale and offset,
frozen geometry, no sectioning, missing from screenshots only. `--offscreen`
cannot show the viewport ones at all, because it has no panels, so the
sub-region between them is the whole framebuffer.

A conventions gate now covers the viewport list (`d6dade3`). The other seven
lists are unguarded. The structural fix is planned as **R1-R5** in
[rendering/renderer-wiring.md](rendering/renderer-wiring.md) — a scene-renderer
registry, then passing the frame state at `Render` time so a missing wire is a
compile error. ~1 session; the six renderers already share identical
signatures and all three render blocks already agree on order, which is what
makes it cheap.

---


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

## An assembly resync re-tessellates every part it draws

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

**What is left** is the tessellation: every resync re-meshes each distinct
part through OCC even when nothing about that part changed. A mate solve, an
explode edit, a visibility toggle and a part edit all pay it.

**Likely fix:** cache the per-part tessellation (vertices, indices, edge
polylines and both renderer assets) keyed by the part document's version, so
a resync re-meshes only what actually regenerated. The lifetime is the
fiddly half — the assets outlive the instances that reference them, so the
cache has to hold a reference and drop it when the part changes or the tab
closes.

**Not doing:** the "visibility-only fast path" this entry used to propose
(route a hide onto `SetInstanceVisible` and skip the rebuild). With the
rebuild an order of magnitude cheaper, the remaining win is small and it
would put a second, differently-shaped code path through the sync — which is
where staleness bugs live.

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

