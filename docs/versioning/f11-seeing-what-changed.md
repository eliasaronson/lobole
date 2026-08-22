# F11 — Seeing what changed — design

> **SHIPPED 2026-08-22** — G1, G2 and G3. Three items on one axis: a history
> you can read, a diff you can look at, and a cost F7 left behind. The
> sections are the design as written, with "What shipped" at the end.

## Why this exists

The version system works and is invisible. After a day's work the History
panel is sixty rows saying `save`:

```
History
  Marsliden dock                          Up to date
  ─────────────────────────────────────────────────────
  before the hinge rework    2 minutes ago   [Restore]
  save                       5 minutes ago   [Restore]
  save                       6 minutes ago   [Restore]
  save                       8 minutes ago   [Restore]
  save                      11 minutes ago   [Restore]
```

Named versions stand out — that is F6 working. But "get me back to before I
broke the gangway" means guessing which `save` that was, and the panel offers
nothing to guess with. A time machine you cannot navigate is a backup, not a
history.

[F9](f9-collaboration.md) sharpened this: pull a teammate's work and there is
no way at all to see what arrived.

## G1 — a log that says what changed (~1 day)

Every row names what it touched, in the words the tabs use:

```
  before the hinge rework    2 minutes ago   [Restore]
  Extrude "Boss" in Gangway  5 minutes ago   [Restore]
  3 features in Dock         6 minutes ago   [Restore]
  the Screwdriver Set assembly + a drawing
                             8 minutes ago   [Restore]
```

**The machinery already exists.** `ProjectMerge` turns
`parts/Stand/features/15-extrude.json` into "Extrude in part Stand" — that is
precisely what a log row needs, and it is written and tested for the Conflicts
panel. Its `Locate`/`Collection` table is the piece to lift.

Cost control matters, because this runs for every row of a 60-row panel:

- **Paths are enough for most rows, and paths are cheap.** One
  `git log --name-only -n 60` gives every path for the whole page in a single
  process — the panel already spends one `git log` per refresh, so this is the
  same call with one more flag. From paths alone: how many items, of which
  collection, in which document. "3 features in Dock" costs nothing extra.
- **Read a blob only to name a single item.** When a commit touched exactly
  one file, one `git show` names it properly: `Extrude "Boss" in Gangway`.
  That is the common case for a save, and it is one blob read per such row,
  cached with the log.
- Rows are summarised, never enumerated: "and 4 more" rather than a wall.

**Verification:** the summariser is a pure function from a path list to a
string — unit-testable without git, which is where most of the cases live
(one feature, many features, several documents, a drawing, project.json
only, a merge commit). One GUI test that a save produces a row naming the
feature. And F6's `rows_fit_at_a_large_font` guard applies: these strings are
longer than "save", so the column must clip rather than widen the panel.

## G2 — a diff you can look at (~2–3 days)

Select two versions (or a version and now) and see what differs. Three layers,
each built on the one before, and **two of the three are already written**:

1. **Which items changed** — the same data G1 computes, per pair instead of
   per commit. Free once G1 exists.
2. **Which fields changed** — parse both blobs and field-diff:
   `distance 12 → 18`, `name "Boss" → "Mounting boss"`. This is
   `ProjectMerge::DiffFields`, already written and already tested, because it
   is what the Conflicts panel shows. It needs no merge to work — two blobs
   are two blobs.
3. **What the geometry did** — the expensive layer, and the one where
   `compare_geometry` (V4 of the agent-validation plan) genuinely belongs.
   Worth stating plainly, because this doc's own history got it wrong: V4 was
   recommended as a *merge* check and is not one, since a merge has no
   reference to compare against. **A diff has two operands by construction** —
   that is exactly the shape V4 wants. If V4 lands, layer 3 is a small
   addition; without it, layers 1 and 2 are still most of the value.

**UI:** inside the History panel, not a new one. Select two rows (or one, to
compare against now) and the changes replace the list until dismissed. A
second panel for something only ever reached from the first is a tab too many
— and the Conflicts panel earned its own window by appearing unbidden, which
this does not.

**Verification:** field-diff cases are already covered by `[merge]`; new tests
cover version-to-version selection and the "against now" case, including a
dirty working tree.

## G3 — a cost F7 left behind (~1 hour)

`ProjectMerge::Scan` runs a **full recursive tree walk plus a `git`
subprocess every second on a clean project**, because `DrawConflictsPanel`
refreshes before deciding it has nothing to draw. Small here (~1.5 ms on a
54-file project) but it scales with file count and never stops.

Two gates, both reusing what already exists:

- Skip `GitVault::Unmerged` entirely when `InMerge()` is false — that is a
  file-existence check standing in for a process spawn, and when there is no
  merge the answer is empty by definition.
- Only re-walk for duplicate ids when **F8's stamp** says the project
  directory changed. F8 already computes exactly "has anything on disk moved",
  which is the precondition for a new duplicate appearing.

This is a defect in F7's implementation rather than a missing feature, and it
is mine to clean up.

## Decisions (settled 2026-08-22)

1. **The diff lives inside the History panel**, replacing the list until
   dismissed.
2. **G1 reads one blob only for a lone changed item**, bounded to the first 20
   rows.
3. **G2 did not wait for `compare_geometry`.** Layers 1 and 2 stand alone and
   are what shipped; the geometry layer is still V4's to add.

## What shipped, and what building it changed

G3, G1 then G2, 2026-08-22, in that order.

**G3 measured, not asserted.** The scan was replaced by an event trigger off
F8's disk poll, and a gui test counts scans: **0 over 60 clean frames**, where
it had been one per second. The plan also proposed skipping
`GitVault::Unmerged` when `InMerge()` is false; **dropped**, because unmerged
paths can also come from a cherry-pick or stash pop run in the terminal
panel, which leave no `MERGE_HEAD`, and once a scan is rare the saving was a
millisecond against a real correctness hole.

**A positional-initialiser trap, twice in two days.** `LogEntry` gained
`changed`, and `Log` built its entries with a braced list — so the new member
silently re-bound `tagRef` and every named version lost its address. The same
trap `settings::Field` carries a comment about. `Log` assigns by name now,
which is the durable fix; a comment on the struct warns the next person.

**The diff is opened by clicking the VERSION, not a button.** A second button
in the action column overflowed at 1.4× font and F6's `rows_fit_at_a_large_font`
guard said so on the first run — the third time that guard has earned its
keep. Clicking the row costs no width, and is what anyone tries first. It must
NOT be `SpanAllColumns`: spanning puts the selectable over the Restore button
and swallows its clicks.

**A diff belongs to its project.** Opening another project left the panel
showing the previous one's changes, with the version list hidden behind them.
Cleared when the path changes — found because it broke three other tests,
which is the good version of finding it.

**The working tree is the second operand, and that is the everyday question.**
`DiffVersions(from, "")` means "what have I changed since that version",
which needs no second pick. Note for anyone reading the field diffs: they come
from `DiffFields`, whose `ours`/`theirs` are a merge's vocabulary — in a diff
they are the from-side and the to-side, and the sentence says "10 to 18"
rather than "10 vs 18" for that reason.
