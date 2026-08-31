# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

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

### An outline contour dips into the part over a hole running beneath the face

A cross-dowel hole runs parallel to the panel edge, ending ~1.7 mm short of
it. Where the section plane passes through that hole, the outer contour dips
about 4 mm into the panel, and the profile then takes a full-depth bite out of
the edge.

Reproduce: front_wall, Back setup, the 210 mm outline. `cam_verify` reports a
16 mm gouge at setup XY (-104.3, -464.9) — both dowel positions, both edges.
It is NOT tool fit (a 3 mm cutter gouges identically) and NOT
`PickedWallContour` (a targetless "cut out the whole part", which sections the
model instead, gouges at the same point). Cutting only the depth band ABOVE
the dowels, or only BELOW, is clean; the band THROUGH them gouges:

    Z   0.0 ->  -4.0   gouge 0.00     (above)
    Z  -6.0 -> -12.0   gouge 10.00    (through — the dowels are at -8.4)
    Z -14.0 -> -16.0   gouge 0.00     (below)

It is in the section, not the offsetting — but the first diagnosis, "a blind
hole ending close beneath a face merges with the outer boundary", is WRONG and
was written without a reproduction. Built as a unit test it does not
reproduce: a 210 x 100 x 16 panel with one Ø8 hole along X ending 1.7 mm short
of the face sections cleanly at the hole's own height — 2 loops, one outer,
one island, outer extent unchanged. The web survives.

So the real part has something the lone hole does not. The candidate is the
INTERSECTION: on front_wall the cross-dowels run into the eight blind Ø8 dowel
holes near the edges (their faces carry the cross-dowels' `;M20`/`;M22`
modifiers, which is how you can tell), and the blind hole at x = ±97 already
leaves only a 4 mm web to the edge at ±105 — the same 4 mm the contour dips.
The next step is to model BOTH holes and section between them, not to go
looking in the offsetting code.
