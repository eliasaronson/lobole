# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---

## save_project to a .json path silently forks the format contract

Found 2026-08-26 when the W 170 S project was saved as `w170s.json`: a
MULTI-document project is always written in the project-container form
(project.json + parts/ + assemblies/ — correct), so the path became a
directory *named* `w170s.json`; but the git vault initializes only for a
`.dsgn` extension, so the identical directory format saved WITHOUT history
and every `save_project {message}` was silently dropped ("ignored
otherwise", says the doc — for a directory-form save that is a silent
capability loss, not an interchange save). format.md's "the .json file
remains the interchange form" holds only for single-part projects; nothing
says so. **Likely fix:** refuse a `.json` path for a multi-document project,
naming `.dsgn` ("a multi-document project saves as a .dsgn directory; .json
is the single-document interchange form") — the substitute is plausible,
loads fine, and is wrong, which is this repo's definition of a refusal.

## Drawing dimensions land where they collide

Found 2026-08-26: `DefaultDimOffset` placed overall dims through the title
block, off the sheet bottom, and on top of each other — every one of five
manufacturing sheets needed two to four manual `offset` passes before it was
readable, and one auto-placed dim measured between lines nobody intended
(a "78" on the brace sheet that corresponds to no feature). Impact: a
first-try drawing is not presentable, for the GUI's Add-dimension button and
the RPC alike. **Likely fix:** a placement ladder — stack labels outside the
view edge nearest the dim, skipping boxes already taken by views, the title
block, and other labels — plus an overlap validator in `check_drawing` so
regressions fail a gate instead of an eyeball.

## CAM cannot target a round opening — every face pick is planar-only

Found 2026-08-26 (W 170 S baffle CAM). `FindFace` (CamToolpath.cpp) resolves
every pick through `FacePlane`, which fails on curved faces — so
`IsWallFace`, `PickedWallContour` and `PickedFaceRegion` all reject the
cylindrical wall of a Ø73/Ø148 through-hole with "pick a flat face — that
one is curved". "Cut the opening" is therefore impossible for the most
common opening there is. Workaround used: an onion-skin CAM twin (cuts
stopped 0.5 mm short so each opening grows a flat floor to pocket-pick).
**Likely fix:** let FindFace fall through to a curved-wall contour (section
the cylinder at mid-height) — the WallContour machinery after it is
shape-agnostic.

Related paper cuts from the same session: `type: drill` via derive refuses
with "drilling picks cylindrical faces, not a verb" even when `targets`
holds cylindrical faces (the derive path ignores drill targets; explicit
top_z+bottom_z is the undocumented way in); and an explicit `top_z` set to
the PART top rapids through the 1 mm of stock above it — the deriver knows
to use the stock top, the error surface ("rapid through material at line
11") doesn't say which op or height to fix.

## Drawing RPC papercuts: leaked json exceptions, inconsistent distance

Found 2026-08-26. (a) `create_drawing {sheet: "A4"}` and a scalar
`add_drawing_dim offset` die with raw `[json.exception.type_error.*]`
instead of refusals naming the accepted shape (object with size/landscape;
[x,y]) — the refuse-with-what-IS-accepted rule, unenforced at json-shape
level. (b) A `distance` dim from a line to a circle measures to some
silhouette point (144-centre hole → "170"), while `distance_x`/`distance_y`
measure to the circle CENTRE (144 ✓) — same picks, different anchors,
and the wrong one is the default kind. (c) A drawing tab screenshot from
an --offscreen instance is silently blank: sheets are drawn by the ImGui
layer, which offscreen captures exclude — should refuse naming the remedy
(export_drawing, or a windowed instance) rather than return an empty frame.

## Part RPCs use three different addressing conventions

Found 2026-08-26 (W 170 S dogfooding). `add_occurrence` takes a part by
**name**, `set_active_part`/`delete_part` take an **index** into the tab
list, `rename_part` takes `part` (a name) plus `new_name`. An agent building
a multi-part project writes a name→index lookup helper in its first five
minutes (every dogfooding script now carries one). Names can also be
ambiguous — see the next entry — at which point index is the only safe key,
but nothing that ANSWERS in names (add_occurrence errors, contact_report)
carries the index alongside.

**Likely fix:** accept `{name}` OR `{index}` (refusing ambiguous names)
everywhere a part is addressed, one shared resolver.

## Duplicate assembly names are accepted silently

Found 2026-08-26: an assembly script died after `add_assembly` (a later mate
refused); the retry created a SECOND tab named "W170S 30l BR". Nothing
warned; `list_parts` showed both, and index-addressing was the only way to
delete the right one. A part/assembly name that duplicates an existing one
should be refused (the merge machinery already refuses duplicate ids —
0363a3e — this is the same conviction one level up).

## No way to list a part's studies

Found 2026-08-26: study ids come back from `add_study` and from nowhere
else — `get_state` does not include studies, and there is no `list_studies`.
Lose the id (a died script, a new session) and the only recovery is probing
`study_status` over a numeric range. Ids are also per-document, so the same
number means different studies on different parts. **Likely fix:** a
`list_studies` RPC (id, name, kind, has_results, stale) + a `studies`
block in `get_state`; both trivially derivable from the document.

## mesh_study/run_study are synchronous past the client's default timeout

Found 2026-08-26: a modal study on a baffle with six Ø3 pilot holes meshed
to 41k tets; the blocking `run_study` outlived designer_remote's 60 s
default socket timeout, the client died mid-call, and the server finished
anyway — the results were there on reconnect, which LOOKS like a hang
followed by a mystery. `run_study` has `async` + `study_status` polling,
but nothing steers a caller there, and `mesh_study` has no async at all.
**Likely fix:** describe-doc the async path on both, and/or return a hint
when a synchronous solve exceeds ~10 s. Client side: `designer_remote.py`
should take a per-request timeout (and its study/mesh calls a long default)
— the 60 s connection default is what actually bit.

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
