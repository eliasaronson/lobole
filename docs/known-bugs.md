# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---

## A GUI test fails when two tests share a process — and the suite is green

**Symptom.** `terminal/off_by_default_docks_right_on` fails when it runs after
`cam/tool_window_follows_the_viewport` **in the same process**. Reproduces
deterministically, no load involved:

    tools/gui-tests-xvfb.sh -f "tool_window_follows_the_viewport,\
                                off_by_default_docks_right_on" --serial

**Why it is invisible.** The default run partitions the suite into four shards
balanced by recorded per-test times, and that partition happens to put the two
in different processes: `gui-tests-xvfb.sh` is green, `--serial` is 177/178.
The balance data changes whenever a test is added, so **a green run is not
evidence this is fixed** — and a future test can bring it back without
touching either file.

**Root cause (diagnosed, 2026-08-22).** Not a timeout and not the PTY.
Instrumenting `DrawTerminalPanel` through the failing pair shows the terminal
window IS created, IS active and IS docked — with `Hidden` true, so `Begin()`
returns false, the body is never submitted, and the widgets inside cannot be
found. `WindowInfo` also declines to return a hidden window, which is the null
the assertion sees. That CAM test is the only one that turns the terminal
panel on and off *and* rebuilds the dock layout (legitimately — it narrows the
viewport).

**Four fixes tried and rejected**, recorded on the test so nobody repeats
them: starting from a rebuilt default layout; waiting on `FindWindowByName`
instead of `WindowInfo`; `ctx->ItemClick(win->TabId)`; poking
`DockNode->SelectedTabId`. None makes the body submit. The remaining suspect
is **which node the window lands in after a layout rebuild**, not when.

**Impact.** Test-only today — no user-facing symptom is known. But the same
mechanism (a docked window left `Hidden` after a layout rebuild) is what
`ui/gui-test-isolation.md` found re-homing the terminal into the *central*
node, where it covered the whole viewport; that one was a real product bug.
Worth treating as a live lead, not just a flaky test.

**Where.** `src/app/guitests/GuiTestsTerminal.cpp` (diagnosis in the test's own
comment), [versioning/f7-handoff.md](versioning/f7-handoff.md),
[ui/gui-test-isolation.md](ui/gui-test-isolation.md).

---

The orbit-lag + stale-silhouette pair in the outline renderer was fixed by
the [rendering/outline-perf.md](rendering/outline-perf.md) work (precomputed
edge adjacency, per-frame-in-flight edge buffers, settle-pass policy),
covered by `tests/test_outline_edges.cpp`.

The preview-handle miss regression was fixed in `PreviewHandles.cpp` and is
covered by the `fillet_body_edge_via_click` GUI test.
