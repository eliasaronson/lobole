# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

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

`conflicts/resolve_a_merge_in_the_panel` failed once on 2026-08-25 in a
sharded run — `panel != nullptr`, the Conflicts panel not found — and passed
alone, and passed on the next full run of the same binary. That is the second
instance this month of the same SHAPE: a GUI test that fails only in company,
where the 4-shard partition decides who its company is.

The first (`terminal/off_by_default_docks_right_on`) was diagnosed to a window
left `Hidden` after a dock-layout rebuild, went away without anyone
identifying the fix, and is withdrawn below. This one is not diagnosed at all.

**What would actually help**, in order: `--gui-test-shuffle` exists and CI runs
it (`d1396b5`), so the cheap move is running the suite shuffled a few times
and keeping the seed of a failure — the failures are reproducible per seed,
which is the whole point of the flag. Chasing one from a single observation is
what the terminal case shows does not pay.

---

The orbit-lag + stale-silhouette pair in the outline renderer was fixed by
the [rendering/outline-perf.md](rendering/outline-perf.md) work (precomputed
edge adjacency, per-frame-in-flight edge buffers, settle-pass policy),
covered by `tests/test_outline_edges.cpp`.

The preview-handle miss regression was fixed in `PreviewHandles.cpp` and is
covered by the `fillet_body_edge_via_click` GUI test.
