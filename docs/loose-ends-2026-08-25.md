# Loose ends — finishing what was started — plan

> Status: **signed off, in progress** (2026-08-25). L1 shipped; L2-L7 open.
> Scope agreed with Elias: the six
> loose ends plus the U5 visual-polish tail. The four deferred follow-ons that
> were also on the table — per-occurrence part-config selection, GPU5 phase 0,
> the Flathub submission tail, and cutting a part on the mill — stay where they
> are; see "Deliberately not in this round".

## Why this doc exists

An audit of all 105 docs under `docs/` against the source, on 2026-08-25. It
was looking for *planned work that was never finished* — as distinct from a
deferral, which is a decision and needs no plan. It found six such items and
six stale status lines. The stale lines are already fixed (`e620203`); this
doc is the six items.

The audit is worth recording because of how one of the six was found.
`interchange/import-plan.md` described 3D splines as a Designer gap, so nobody
re-read the Onshape importer's `fitSpline` skip — which had been dead code
since `Curve3D` shipped as format v42. `feature-gaps-plan.md` had even written
down the step that was missed: *"after each ships, update the importer to
replay it."* **A stale status line does not just misinform; it makes the thing
behind it invisible**, because the reason on the skip still reads true.

The other five are not hiding behind anything. They are simply unowned.

## What is in, and the one thing they have in common

| | Item | Effort | Where it came from |
|---|---|---|---|
| **L1** | The importer's dead `fitSpline` skip | ~1 h | this audit |
| **L2** | `describe` grows a parameter schema, with a gate | ~1 session | three dogfooding rounds |
| **L3** | The GUI test that fails only in company | ~½ session, timeboxed | `f7-handoff.md` |
| **L4** | Reference images, finished | ~1 session | the only doc still in flight |
| **L5** | The coverage-asymmetry matrix | ~1 session | roadmap "What's next" #3 |
| **L6** | Two papercuts | ~2 h | dogfooding, `direct-manipulation.md` |
| **L7** | U5 — the visual-polish tail | ~1–2 sessions | roadmap "What's next" #4 |

Four of the seven (L3, L4, L5, L6) are the **GUI/interaction layer**, which is
exactly where the roadmap says every recent defect has come from and where the
RPC-driven smokes structurally cannot reach. That is not a coincidence and it
is the argument for doing them together: the same reset/addressability
machinery is in play in all four, and L5 produces the map the others fill in.

---

## L1 — The importer replays a `Curve3D` instead of skipping — **SHIPPED**

`tools/onshape_import.py:2246` reports *"fitSpline: 3D curve object not in
Designer yet"*. `Curve3D` has existed since format v42 — `CurveFeature` with
`Curve3DKind::FitSpline`, and a row in `kFeatureTypes`
(`SerializeFeatures.cpp:924`), so `add_feature` accepts it.

- Replace the skip with a mapping: Onshape's `fitSpline` points → a `Curve3D`
  feature with `kind: fit_spline`, `closed` from the source.
- Report it as `mapped`, not `skipped`, so the round-trip report stops lying.
- **Leave `splitPart` alone.** Its skip is NOT stale: the knife is an
  unresolved Onshape *query*, which is a real remaining gap, and the comment
  already says so correctly.

**Verification:** a `replay-dump` fixture containing a 3D fit-spline replays
into a document whose feature list contains the `Curve3D`, with the fit points
round-tripping. Zero API calls — `replay-dump` reads a saved JSON dump.

**Risk:** low. The only judgement call is Onshape's tangent/end-condition
data, which `GeomAPI_Interpolate` takes optionally; if the fixture's spline
carries tangents that do not map cleanly, import the points and say so in the
report rather than inventing an end condition.

**As built.** No fixture in the repo contains a `fitSpline`, so verification is
a unit test of the pure mapping (`map_fit_spline`) plus a check that the
**dispatch calls it** — which is the check that would have caught the original
defect, since the skip lived in the feature-type if/chain and nothing asserted
what that chain did. All three sabotage-checked. Two things came out
differently from the sketch above:

- **Sample the built curve, do not resolve the through-point query.** Onshape's
  fit points are a query of vertices/mate connectors, and this importer has
  learned three times over that queries do not resolve at rollback
  (`up_to_verts`, `consumed_edges`, `face_diffs` all measure geometry
  instead). A new `Client.curve_sample_points` walks the created edge with
  `evEdgeTangentLine`; re-fitting through a dense sample of a spline gives back
  that spline. Both sources gained `curve_points`, and `cmd_dump` records it,
  so `replay-dump` maps it offline like everything else.
- **Periodic splines repeat their seam point.** Designer's `closed` flag
  re-closes the loop, so passing the duplicate through would pin a zero-length
  span across the seam. Dropped — but only when the first and last points
  actually coincide, since dropping unconditionally would silently shorten a
  curve. Both halves are tested.

**Still unverified:** the live path. There are no Onshape credentials here and
the API has a call budget ([onshape-terms.md](interchange/onshape-terms.md)),
so `curve_sample_points`' FeatureScript has not run against a real document.
The pure mapping under it is covered.

## L2 — `describe` grows a parameter schema, and a gate that keeps it true — **SHIPPED**

**The evidence.** `dogfooding.md` records the **third** separate agent reaching
for `describe {method: …}` and finding it absent. `remote::MethodDoc` is
`{name, doc}` with the params in prose (`RemoteHelpers.h:22`); only
`feature_types[]` carries a real `params` list.

This is worth more than its size. Dogfooding is the top source of defects in
this repo, so anything that makes a round cheaper compounds — and an agent that
has to guess a parameter name spends its round discovering the API instead of
finding bugs.

**Why the obvious mechanism does not work.** `RpcParams::SeenKeys()` already
records a method's exact accepted vocabulary — but only by *running* the
handler. The unknown-key check is deliberately post-execution
(`RemoteServer.cpp:1938`, with a comment explaining the trade), so a
runtime-derived schema would have to execute every method, including the
destructive and the slow ones. Out.

**What to build instead**, which is a pattern this repo already has twice:

1. A `params` field on `MethodDoc` — one line per method, beside the branch it
   documents, exactly where `doc` already lives.
2. `describe {method: "…"}` returns that one method (today `describe` takes no
   arguments and returns everything).
3. **A source-scanning drift gate**, in the shape of `tests/test_conventions.cpp`
   — which already walks `src/` via `DESIGNER_SOURCE_DIR` and regexes it. The
   scanner splits each domain module on its `m == "…"` branches and extracts the
   `p.at("…")` / `p.value("…", …)` / `p.contains("…")` literals in each, then
   fails when a branch and its table row disagree **in either direction**. That
   is stage 4 of the architecture review applied one level out: a new RPC
   without a params row fails on its first run, rather than quietly documenting
   nothing.

**Risk — and it is the real one here.** The scanner has to attribute a key
literal to a method, and two shapes defeat it: a handler that reads params in a
helper function, and a branch that serves several methods. Mitigation: an
explicit `// rpc-params: opaque` marker that opts a branch out and is itself
greppable, so the exceptions are a visible list rather than silent gaps. If the
exception list comes back long, that is a finding — say so and ship the table
without the gate rather than pretending the gate covers everything.

**Verification:** the gate checked against deliberate sabotage in both
directions (add a `p.value` the row does not list; list a param the handler
does not read), which is the standard this codebase holds its gates to.

**As built.** `MethodDoc` gained a `params` field, all **195 rows** filled;
`describe {method: "..."}` narrows to one and refuses an unknown name;
`tools/rpc_params_scan.py` reads the handlers and `tests/rpc_params_gate.py`
diffs the two (a ctest, source-only, 0.1 s, no app and no GPU). Sabotage-checked
three ways — a key the handler reads with no row, a row naming a key nobody
reads, and a new branch with no row at all.

**The risk this plan flagged came in far below estimate: 195 of 195 branches
attribute cleanly, and only 3 need the opaque marker** (`add_constraint`,
`add_constraints`, `set_settings` — all genuine `p.raw()` free-form specs).
The plan's fallback ("if the exception list comes back long, ship the table
without the gate") was not needed. Four things made the difference, and the
first two are findings in their own right:

- **Brace-aware scoping, not a regex sweep.** Handlers group methods that
  share a preamble — five occurrence edits read `id` once, then dispatch
  inside — so splitting the file at each `m ==` mention misattributes **35%**
  of branches. Tracking each method scope's brace depth and asking which
  scope encloses a key most tightly gets it right.
- **Two branches were labelled by a comment the compiler could not see.**
  `} else { // rename_occurrence` and `} else { // delete_reference_image`.
  The scanner attributed their keys to every method in the group, which would
  have advertised `name` on `set_occurrence_visible` — a parameter the
  dispatcher then refuses, which is worse than saying nothing. Both are now
  `else if (m == "...")`: the code says what the comment said, and it is
  equivalent because each outer group enumerates exactly those names.
- **`screenshot` and `render_sheet` are staged asynchronously**, before the
  `RpcParams` path exists, so they match on `request.method` and read
  `request.params` directly. Worth knowing beyond this gate: **the strict
  unknown-key rejection does not cover those two.**
- **An explicit `// rpc-params: a, b, c` marker** for keys read where the
  scanner cannot follow — a lambda taking the key as an argument
  (`set_units`), or a helper constructor (`ItemTarget`, which reads `ref` and
  `id` for the six `item_*`/`popup_*` actions). Greppable, so the exceptions
  are a visible list rather than silent gaps.

**The gate caught its own parser bug, which is the best evidence it works.**
`clang-format` reflowed four long rows (`pattern`, `query_geometry`,
`set_configuration`, `set_study`), splitting their params list across adjacent
string literals — which C++ concatenates and the first version of the gate did
not. It read only the trailing literal, so those four rows became a *prefix*
that still looked like a perfectly valid key list. The scanner disagreed with
the table and the test went red. The row parser now splits on top-level commas
and joins each field's literals.

**The payoff, concretely.** `describe` now answers
`check_geometry → params: ["body"]`, and sending `tolerance` is refused with
*"unknown parameter 'tolerance' for 'check_geometry'. Accepted: body."* Those
two agree because the gate forces them from one source of truth — previously
the second existed and the first did not, so the only way to learn a key name
was to send a wrong one.

## L3 — The GUI test that fails only in company — **SHIPPED** (it was already fixed)

`terminal/off_by_default_docks_right_on` fails when it runs after
`cam/tool_window_follows_the_viewport` in one process. The default 4-shard run
separates them and is green; `--serial` is 177/178. Now recorded in
[known-bugs.md](known-bugs.md).

**Do this in three steps, and stop at whichever one succeeds.**

**Step 1 — test the one hypothesis left, timeboxed.** Four fixes are recorded
as tried and rejected on the test itself; do not repeat them. What has *not*
been tested is the mechanism named as the remaining suspect: **a window that
survives a layout rebuild holding a stale `DockId`**. Note that
`ResetAppForTest` already sets `buildDefaultDockLayout = true` unconditionally
(`GuiTestsCommon.cpp`), so "the layout was not reset" is already disproved —
the layout *is* rebuilt, and the window comes back `Hidden` anyway.

The question that decides everything: **does a person hit this?** Turn the
terminal on, turn it off, `View > Reset Layout`, turn it on again.
`ui/gui-test-isolation.md` found this exact family once already — a rebuild
with the terminal preference off deleted its node and ImGui re-homed the
window into the *central* node, where it covered the entire viewport, and that
was a genuine product bug found through a test failure. If it reproduces by
hand, this stops being a test problem and the fix goes in `TerminalPanel`.

**Step 2 — if step 1 comes back clean, split the test** (Elias's call, and the
right one). Most of what this test asserts is not about ImGui at all, and
`StartTerminalCommand` / `TerminalRunning` are **free functions on `AppState`
with zero ImGui calls** — verified. So:

- *Headless*, into `tests/test_actions.cpp` against the `designer_app` library:
  off by default; turning the preference on starts a shell; **turning it off
  and on again works** — which is the assertion with real bug history
  (`2eda31d`, the `everStarted` latch). The architecture review measured this
  trade: the GUI equivalents cost ~1 s each and need a GPU, the headless ones
  total milliseconds and check more.
- *GUI*, kept and narrowed: it docks, and it does not float over the model
  tree. Assert this through `FindWindowByName` + `DockId` rather than
  `WindowInfo` — **the failing assertion is the one that needs the window's
  body submitted** (`###term_claude`), not the docking one, and the diagnosis
  on record says the window IS created, active and docked with `DockId 4` even
  in the failing configuration. The body-widget assertion belongs to a test
  that opens the panel on purpose, not to one named "off by default".

**Step 3 — the shard pin, and only as a last resort.** Elias's reaction was
*"that feels ugly"*, and it is: pinning two tests into one shard hard-codes a
workaround into the partition that the runner otherwise derives from the
binary itself. Take it only if steps 1 and 2 both fail, and if taken, it is a
red test, not a hidden one.

**The deliverable is that the suite stops lying** — reachable at any of the
three steps.

**As built — step 1 answered a different question than expected.** The failure
**does not reproduce**. The full GUI suite passes **211/211 under `--serial`**,
which is the check the original note named as the real one, and so does the
documented two-test pair. It was not sidestepped: the CAM test still toggles
the terminal preference and rebuilds the dock layout twice, exactly as before.

**The fixing commit was not identified, and that is stated rather than
implied.** `b1f0330` (`NoTabBar` instead of an uncleared
`AutoHideTabBar|HiddenTabBar` on the panel's node) was the obvious candidate
and matched the diagnosis well — a node with a hidden tab bar and two windows
would leave the unselected one's body unsubmitted, which is the reported
symptom exactly. It is not it: reverting that hunk on top of HEAD does not
bring the failure back. A bisect to 2026-08-21 then found the pair passing at
every commit sampled — and that bisect had its good/bad arguments inverted, so
it proves only that, not where the transition is. Somewhere in 08-22..08-24
something fixed it without announcing itself.

**What the round leaves behind is worth more than the bisect would have been:**

- **`terminal/survives_a_dock_layout_rebuild`** — the behaviour is now NAMED.
  Until this, the only thing exercising a layout rebuild against this panel
  was `cam/tool_window_follows_the_viewport`, which does it as a side effect
  of testing something else. Inheriting coverage from another domain's test is
  precisely how the original stayed hidden: nothing failed for the right
  reason. The new case asserts a widget *inside* the panel, which requires
  `Begin()` to have returned true — the thing that actually broke — across a
  rebuild while on, while off, and back. **Honest limitation: it is a
  characterization test, not a verified regression test.** No sabotage I could
  construct reproduces the original, because the original no longer exists at
  any commit I can build.
- **The lifecycle half moved headless** (`tests/test_actions.cpp`), as Elias
  asked: `NewTerminalTab`, `ShutdownTerminal` and `TerminalRunning` are free
  functions over `AppState` with no ImGui in them. Milliseconds, no GPU.
- **A dead latch, deleted.** Writing that test turned up that `everStarted` —
  the field at the centre of the 2eda31d fix — is written and **never read**,
  while `ShutdownTerminal`'s comment and a GUI test's comment both still
  described it as load-bearing. Forgetting the sessions is what carries the
  behaviour, and the new headless test pins it by session count
  (sabotage-verified: kill-without-forgetting fails it).
- **An orphaned comment reunited with its test.** The explanation of the
  reopen bug sat in `GuiTestsViewport.cpp` above an unrelated `handles` test:
  the `RegisterGuiTests` split moved the test and left its comment behind.
  That refactor verified test NAMES byte-identical — nothing checks that a
  comment travelled with the code it describes.

**The shard pin was not needed**, which is the outcome Elias preferred.

## L4 — Reference images, finished — **SHIPPED**

[sketching/reference-image-plan.md](sketching/reference-image-plan.md) is the
only workstream doc still genuinely in flight, and has been since 2026-07-21.
Two things are outstanding and they compound:

- **Phase 4 was never started:** calibration-point overlay markers, a
  main-menu entry, undoable opacity, tests.
- **Phase 3's GUI has never been driven end to end.** Its own banner says so:
  the panel and the calibrate tool are unit- and RPC-verified only. The
  feature a person actually touches has no click coverage.

Do them together, because phase 4's "tests" and phase 3's missing check are the
same work. The blocker recorded at the time — *ImGui does not register items in
an inactive dock tab* — has since been solved by `focus_panel`, added during
the CAM round.

**Verification:** a GUI test that inserts an image, calibrates it by picking
two points and typing a distance, and asserts the scale changed; opacity is one
undo step, not one per slider frame (the `HandleEdit` recipe).

**As built. "Polish" was the wrong word for one of these.** Opacity had no undo
entry, so adjusting it and pressing Ctrl+Z popped the command underneath —
the image insertion. **The image disappeared.** The sabotage check is what
made it plain: reverting the fix fails the new test on `images.size() == 1`,
not on the opacity value. A persisted field with no command is not a missing
nicety, it is an undo stack that lies about what it will do.

The rest as planned: calibration markers (the tool asked for two points and
drew nothing, so a mis-picked point silently rescaled everything traced over
it), a `File > Reference Image…` entry gated by the same command predicate as
the toolbar button, and `sketch/reference_image_panel_by_clicking` — **the
first test to touch this feature at all**, sabotage-checked both ways.

Two things worth knowing for the next feature like it:

- **Insert stays unclicked, deliberately.** It opens a native file dialog,
  which the test engine cannot drive — the same reason Import has no clicked
  coverage (`docs/ui/gui-coverage.md`). The action behind it is called
  directly and everything after it is clicked, which is where the panel is.
- **GUI tests can now read checked-in fixtures.** `DESIGNER_SOURCE_DIR` was
  defined for the test binary only; it now rides the test-engine branch of
  `designer_app` too. The fixture is 87 bytes and deliberately non-square, so
  a test can tell a width from a height.

## L5 — The coverage-asymmetry matrix — **SHIPPED**

Roadmap priority: *"for each interactive tool, does a test exercise it on every
tab kind it claims to support?"* Never run as a deliberate pass, while the
evidence kept arriving — the section arrow (twice), and F9's whole sharing
surface, which was part-gated and failed on an assembly tab (`9048d58`).

**The matrix is the deliverable**, even if the fixes are follow-ups: tools down
one axis, tab kinds (part / assembly / drawing / CAM) across the other, each
cell either a test name or a gap. Produce it from the binary's own
`--gui-test-list` plus the tool registry rather than by reading tests, so it
can be regenerated rather than re-audited — the same discipline that makes the
shard partition trustworthy.

Fill the cheap gaps in the same pass; file the expensive ones with the matrix
as their evidence.

**As built** — `tools/coverage_matrix.py` → [ui/coverage-matrix.md](ui/coverage-matrix.md).
The test list comes from `designer --gui-test-list`, so a renamed or deleted
test cannot linger; everything else is a source scan against hand-written
marker tables, which is a heuristic and says so.

**Result: nothing a tool supports is completely untested.** The interesting
part is what that did NOT mean, and it changed how the tool works:

- **A zero-only check would have found nothing.** Every historical bug of this
  shape had *some* assembly coverage — the section arrow had an assembly test
  when it broke on assembly tabs the second time. So the matrix flags **thin**
  cells too: covered, but far less than the same tool's best-covered kind.
- **The count then over-flagged, and tuning the threshold until it agreed with
  me would have been fitting the tool to the answer.** Measure and Section on
  assembly looked thin at 2 tests each — and reading them showed
  `offset_arrow_drags_on_an_assembly` *is* the regression test for the bug
  this matrix exists because of, and `whole_bodies_and_parts` asserts the
  measure overlay on an assembly tab, which is exactly what measure-v2 found
  missing. Two tests aimed at the two known failures beat five aimed at
  nothing. Those cells are now `REVIEWED` in the generator, with the reason,
  so they stop re-flagging and what is left standing is the real question.
- **CAM is not a tab kind.** Listing it as one made every CAM test count as
  "not on a part tab", so the first run reported the CAM tool untested on the
  only tab kind it runs on — a matrix confidently saying the opposite of the
  truth, which is the exact failure it exists to prevent. CAM is a tool
  *session* on a part tab, as Simulation is a *mode* on one.
- **A test family registered from a table has no literal name to scan.** The
  24 `defaults/*` cases read as missing until the generator learned to expand
  a family against the binary's list.

**One gap filled:** `tabs/an_assembly_tab_folders_like_a_part`. Folders live on
`PartSlot` and every tab kind is a `PartSlot`, so the code says this cannot
break — but that is an argument from structure, and both bugs this matrix
exists because of were "the code obviously handles both tabs" until someone
clicked. It now clicks, including through a save/open round trip.

**Filed, not fixed:** the feature tree (38 part / 3 assembly / 2 drawing) and
the tab bar (5 / 2 / 2). Both are real asymmetries; neither is a known
failure, and inventing tests to level a ratio is how a suite gets slower
without getting better.

## L6 — Two papercuts — **SHIPPED** (one was already fixed, and untested)

- **The prompt pill and the Tool window overlap** at the top of the viewport —
  the Hole tool's "Step 2/3" pill runs underneath its own dialog
  (`dogfooding.md`, drawings round). One prompt-pill renderer exists (U7), so
  this is one placement rule, not a per-tool fix.
- **No revolve drag GUI case.** `ui/direct-manipulation.md` lists it as item 1
  of its own remaining order; extrude and fillet have theirs. The doc already
  contains the recipe (project the handle tip with `ui::WorldToScreen`, drag
  along the projected axis, assert the angle changed and the preview survives).

**As built.** The prompt-pill overlap turned out to be **already fixed** —
`DrawPromptPill` clamps against `toolWindowRect`, and its comment describes
the exact dogfooding symptom. The same class as L1's importer skip: a closed
defect still listed as open, which is what a doc audit is for. What was
missing was a test, and it is arithmetic drawn to a foreground draw list that
no GUI test can address — so the geometry came out into a pure
`PromptPillRect` and is pinned in `test_actions.cpp` instead. That is the
trigger the architecture review names for a headless test beside a GUI one.

**The first version of that test was worthless, and the sabotage check is
what said so.** Its tool window was 260 px wide, which does not reach a
centred 300 px pill — so the assertion passed with the clamp deleted. It now
uses a 420 px window (the Hole tool's rough width, the case the dogfooding
round actually hit) and `REQUIRE`s that the unclamped pill *would* have
overlapped, so the check cannot go quietly inert again.

The **revolve handle** is now `handles/revolve_arc_drag_sets_angle`. It is the
only one of the three that ROTATES, so it exercises `RayPlane` +
`RotateAngle` rather than `RayAxisParam` — a half of the drag math with no
coverage at all. Two details worth keeping: the grab point is read off the
DRAWN handle geometry rather than recomputed (`PerpTo` and the axis
resolution are file-local, and a test that re-derives them can drift from
what is on screen), and the drag is NEGATIVE because the default angle is
360, which is the clamp ceiling — a positive drag would be swallowed and the
test would pass against a handle that did nothing. Sabotage-checked both
ways.

## L7 — U5, the visual-polish tail (~1–2 sessions)

The oldest item on the roadmap's next-list and the thing a new user sees first.
From `ui/plan.md` §U5: spacing/padding rhythm audit, **error/suppressed tree
badges beyond colour**, shaded region fill, whole-instance occurrence hover,
tooltips audit. Plus §U4's leftover: audit remaining screen-space pixel
constants.

**U13 is not this**, despite adjacency — it did icons, overflow, elision and
units. What is left is rhythm and state legibility.

**Scope discipline, because this is the fuzziest item here.** Two of these are
correctness-shaped and should lead: **badges beyond colour** (an errored or
suppressed feature currently differs by hue alone, which is a legibility floor,
not a polish item) and the **tooltips audit** (U13 added `label_truncated` as a
DebugLabel signal — a truncated label without a tooltip is now *detectable*, so
this one gets a gate rather than an opinion). Spacing rhythm and shaded region
fill are taste; timebox them and stop, rather than iterating on screenshots.

---

## Sequencing

L1 → L2 → L3 → L4 → L5 → L7, with **L6 riding along** with whatever is in
flight (two hours, and it will otherwise stay orphaned for another month —
which is how it got here).

The rationale, since it is not simply cheapest-first: **L1 costs an hour and is
this audit's own finding**, so it closes the loop that justified the round.
**L2 is first among the real work because it compounds** — every later
dogfooding round, including the ones that will find the next batch of these, is
cheaper once an agent can ask what a method takes. **L3 before L4/L5** because
it is a standing untruth in the suite those two will add tests to, and because
it might be a product bug. **L7 last** because it is the largest and the least
falsifiable, and because it is the one a bad week can be dropped from without
stranding anything.

## Deliberately not in this round

Each of these was considered on 2026-08-25 and left where it is:

- **Cutting a part on the mill.** Not code, not this round's shape — but it is
  the roadmap's #1 and the highest-information action available. It runs in
  parallel with all of the above and needs nothing from it.
  `cam/dogfood-round.md` already lists the five things to watch, in order.
- **Per-occurrence part-config selection** — the one named configurations
  follow-on, and the cheapest deferred item because `configurations/plan.md` §2
  evaluates against an override *argument* specifically so this becomes a pure
  read. Still demand-gated: no model has wanted it yet.
- **GPU5 phase 0** — one day, no production code, and a gate that kills the
  plan if its assumption fails. Buys speed, not correctness: the CPU fallback
  shipped, so slender parts are already right, just slow.
- **The Flathub submission tail** — five mostly-mechanical checkboxes that
  separate "we built a Flatpak" from "anyone can install it". Worth doing the
  next time distribution matters; nothing here depends on it.
