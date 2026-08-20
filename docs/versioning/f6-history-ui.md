# F6 — Version history in the app

## Why this exists

[F5](plan.md#f5--git-integration) shipped the *engine*: a `.dsgn` project is
its own git repository, every clean save is a commit, and `GitVault` can log,
tag and restore. What it did not ship is any way to see or use that from the
app. `src/ui/` calls exactly one GitVault function — `CommitAll`, from
`MarkProjectSaved` — so the history exists, silently, and is reachable only
over RPC (`history_log`, `history_restore`) or from a terminal.

The visible consequence is `File > Checkpoints`, which predates git and still
uses the F4 mechanism: full-document JSON copies under
`<project>.checkpoints/`. On a multi-part project that mechanism is now
**wrong, not merely old** — it snapshots `*state.document`, the *active part*,
so "create checkpoint" on the dock project saves whichever tab happens to be
open and silently omits the assembly, the other 48 parts and the drawings.
Restore then replaces that one part. The same bug sits in the
`checkpoint_create` RPC, which additionally tags git and so does *both* things
under one name.

So this is not a new feature so much as finishing one and removing the older,
weaker copy of it.

## What a user gets

A **History** panel, tabbed beside Model / Variables / Configurations and
present on every tab kind (a version is a property of the project, not of the
part you are looking at):

```
History
  Marsliden dock            ● unsaved changes    [Save]
  ─────────────────────────────────────────────────────
  Name this version...
  ─────────────────────────────────────────────────────
  ▸ before the hinge rework      2 minutes ago   [Restore]
    save                         5 minutes ago   [Restore]
    save                         1 hour ago      [Restore]
    checkpoint: gangway v2       yesterday       [Restore]
```

Named versions are commits carrying a tag; unnamed rows are ordinary
save-commits. One list, one restore, no second mechanism.

### It speaks CAD, not git

The panel says *version*, *name this version*, *restore*. It never says HEAD,
ref, detached, or hash. Git is the storage engine, the way OCC is the geometry
engine; a user who wants git has a terminal, and one who does not should still
get "go back to how it was on Tuesday". The one place the underlying tool is
named is the refusal when it is missing (below), because there the user has to
act on it.

### Restore keeps history linear

Restoring writes the old tree over the working directory and **commits that as
a new version on top**. Nothing is lost and there is no detached state to
explain: the list only ever grows downward, and "restore, look, restore back"
works because the state you left is still in the list.

## What has to change under it

### 1. `GitVault::RestoreTo` is wrong today

It runs `git checkout <commit> -- .`, which restores the paths *present in that
commit* and leaves everything else alone. A file added after that commit
survives the restore. Verified directly:

```
commit one: a.txt
commit two: a.txt b.txt
git checkout HEAD~1 -- .   ->  a.txt b.txt     # b.txt should be gone
git read-tree --reset -u HEAD~1  ->  a.txt     # correct
```

For a project that means restoring to before a part existed leaves that part's
directory on disk, and the reopened project still lists it. The fix is
`read-tree --reset -u`, which sets index and working tree to the target tree
and deletes what is not in it, while leaving HEAD alone — exactly the linear
semantics above. This is reachable from `history_restore` today, so it is a
live bug, not a new-code risk.

### 2. `LogEntry` needs tag names, and `Dirty()` needs to exist

`%D` in the log format carries ref decorations; parsing `tag: X` out of it
gives each row its name in one call rather than a second `git tag` invocation
per commit. `Dirty()` is `git status --porcelain` non-empty — the panel's save
indicator, and the guard on restore.

### 3. Checkpoints become named versions, in both paths

- The `File > Checkpoints` submenu is replaced by a `File > History` item that
  focuses the panel. The part-only JSON snapshot path is retired.
- `checkpoint_create` saves the **project** (`ui::SaveProject`) rather than
  `DocumentIO::Save(*doc, …)`, then tags. `checkpoint_list` returns tags.
  `checkpoint_restore` restores the tagged commit. The RPC names stay — they
  are public surface with tests and a skill that uses them — but they now mean
  the same thing the panel means.
- Without git, `Checkpoints::` remains as the fallback, and the panel says so
  in one line: *"Version history needs git on PATH."* No silent no-op.

## Two things found while building it

**`AddPart`/`DeletePart` never marked the project dirty.** Which documents a
project *has* lives in `project.json`, not in any document's version, and
`ProjectDirty` only compared document versions — so adding a tab left the
project reading "saved", and closing lost it with no prompt. It surfaced here
because the panel's save indicator and its restore guard both ask
`ProjectDirty`; a version feature is only as honest as that answer.

**Bringing a docked tab forward takes a repeated request.** `File > History`
has to select the panel's tab, and neither `SetWindowFocus` nor one
`TabBarQueueFocus` does it: ImGui's `FocusWindow` deliberately skips dock-tab
selection (`// For #2304 we avoid applying focus immediately before the
tabbar is visible`), and `Begin` returns **true one frame before** an
about-to-be-selected tab is actually up — so the obvious "stop asking once we
are visible" drops the request a frame early and the tab never appears.
Measured directly: 2 frames of asking does nothing, ~1 s works. The panel
therefore re-issues the request for a bounded 60 frames.

Worth knowing because the first version passed a GUI test while doing nothing
in the running app. The test only became honest once it asserted the *tab
selection* (`DockNode->SelectedTabId == TabId`) before and after the click,
rather than "the window exists".

## Deferred, with reasons

- **Branch and switch.** In the F5 list, but a branch raises "what happens to
  my open document" and "how do I get back" — the questions linear restore
  exists to avoid. Cheap to add later on top of this panel; nothing here
  forecloses it.
- **Open a commit read-only.** `git worktree add` to a temp dir and open that
  as a project is genuinely cheap *because* projects are directories, but it
  doubles the open-project state (two projects live at once) for a case
  "restore, look, restore back" already covers.
- **Conflict resolution UI** (pick mine/theirs per feature). Unchanged from
  F5's own note: a semantic feature-list merge is a separate project. The
  merge-safety work of 2026-08-19 (duplicate-id refusal, named parse errors)
  is the current answer — a conflict is refused loudly rather than resolved
  quietly.
- **Discarding uncommitted changes** ("revert to last save"). One more
  destructive button; the panel disables Restore while dirty and says to save
  first, which is one keystroke.

## Verification

- Unit (`tests/test_gitvault.cpp`): init/commit/log; a tag shows up on its
  entry; `Dirty()` tracks a scratch edit; **restore removes a file added after
  the target commit** (the bug above, as a regression test).
- Unit: a two-part project, checkpoint, delete a part, restore — the deleted
  part comes back and the project reopens with both. This is the case the old
  part-only checkpoint could never pass.
- GUI (`history/*`): the panel exists on a part tab and on an assembly tab;
  naming a version adds a row; Restore is disabled with a reason while dirty.
- `tools/test-fast.sh` in the loop, full `ctest -LE gui` plus
  `tools/gui-tests-xvfb.sh` before each commit.

No format change: git objects are not the document format, and `kVersion`
does not move.
