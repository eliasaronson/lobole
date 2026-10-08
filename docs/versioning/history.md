# Version history, merging and sharing

A `.dsgn` project is its own git repository, and every save is a commit.
This page covers what the app does with that: the History panel, named
versions, branches and merges, the Conflicts panel, remotes, and seeing what
changed between versions. How a project is laid out on disk is in
[README.md](README.md).

Git is the storage engine, the way OpenCASCADE is the geometry engine. The
version list speaks CAD — *version*, *name this version*, *restore* — and
never shows a hash, HEAD or a ref. The sharing and branch controls use git's
own words (Remote, Fetch, Pull, Push, Branches, Checkout, Merge), because
their failures come back from git in git's vocabulary.

## The engine: GitVault

`src/core/io/GitVault.{h,cpp}` shells out to the `git` binary. Every call is
a no-op, or answers "nothing", when git is not on `PATH`.

- **Commit on save.** `ui::MarkProjectSaved` initialises the repository on
  the first save of a `.dsgn` path and commits everything with the save's
  message (default `save`; `save_project {message}` sets it). A single-file
  `.json` project has no history.
- **Who commits.** Per field, git's own `user.name` / `user.email`, then the
  Preferences fields *Your name* / *Your email* under Sharing (settings
  `git_user_name`, `git_user_email`), then the inline fallback
  `lobole@local`, so CI and a fresh machine without git config can still
  commit.
- **Quote every format string.** Commands go through a shell:
  `%(refname:short)` is a syntax error and `%an|%ae` a pipe, and the failure
  is an empty answer, not an error.
- **Aggregates grow at the end.** `LogEntry` is filled by name; a member
  added mid-struct to an aggregate built with a braced list silently re-binds
  every field after it.

## The History panel

`src/ui/HistoryPanel.cpp`, a tab beside Model, Variables and Configurations,
present on every document kind: a version belongs to the project, not to
the tab in front.

- **Rows** come from `GitVault::LogWithChanges`: one `git log --name-only`
  for the page, turned into the tabs' words by `ProjectMerge::SummarizePaths`
  ("3 features in part Dock"). A commit that touched exactly one item gets
  one `git show` to name it (`Extrude "Boss" in part Stand`), for at most the
  first 20 rows. The cheap `Log()` is compared first, so the expensive call
  runs only when the log moved, and not at all while the tab is hidden.
- **Name this version** saves the project and tags the commit
  (`ui::NameCurrentVersion`). The tag is annotated, under `cp/<slug>`; the
  name the person typed is the commit subject and the ref is only an
  address. If `cp/<slug>` already exists (two people chose one name), the
  new address gets a short-hash suffix and both names survive. `LogEntry`
  carries the ref it found (`tagRef`), because the address cannot be
  recomputed from the name after disambiguation.
- **Restore** (`ui::RestoreVersion`) writes the old tree over the working
  directory with `git read-tree --reset -u`, which also deletes files the old
  commit did not have, commits that as `Restore "<name>"` on top, and
  reopens the project. History only grows, so "restore, look, restore back"
  works and nothing is rewritten for anyone sharing the branch. Restore is
  disabled while the project is dirty ("Save first").
- **Rows fit at any font scale.** Reserved widths come from `CalcTextSize`
  and the style's padding, never pixel literals; the
  `history/rows_fit_at_a_large_font` test raises the font scale to 1.4 and
  asserts nothing passes the panel's right edge.

Without git, `checkpoint_*` fall back to `src/core/io/Checkpoints`: a JSON
copy of the ACTIVE PART only under `<project>.checkpoints/<name>.json`, and
the reply says `part_only: true`. The History panel says that version
history needs `git` on the PATH and that saving works without it.

## Branches and merging

`GitVault` has local branches (`Branches`, `CreateBranch`, `SwitchBranch`),
`Merge`, `InMerge` (MERGE_HEAD exists), `Unmerged`, three-stage reads
(`ReadStages`), `ResolveWith` / `ResolveWithContent` / `ResolveAsDeleted`,
`AbortMerge` and `CommitMerge`, which refuses while any path is unmerged.
Switch refuses on a dirty tree. Switch, merge, restore and abort each end in
`ui::OpenProject`, so the documents in memory always match the tree.

`core/io/ProjectMerge.{h,cpp}` is what a conflict MEANS in a project. The
directory format produces five classes, and they are not the same pick:

| class | what | resolution |
|---|---|---|
| Item | one feature / mate / dimension file edited on both sides | whole side, or one field at a time (`TakeField`). Rare: git already merges disjoint fields of one item, since the JSON is one sorted key per line |
| Order | an `index.json` id list | three-way union against the merge base (`UnionIndex`): additions from both sides kept, deletions stay deleted. Their additions anchor to the entry that preceded them on their side, then step past ours, because feature order is regeneration order. Picking a side would drop the other side's features from the order |
| DuplicateId | two files claiming one id (`15-box.json`, `15-hole.json`) | git merges this CLEANLY, so it never appears in `Unmerged`; found by walking the tree. Renumbered by `RenumberDuplicate` |
| Project | `project.json` | whole side |
| Derived | `geometry/*` | either side; the chain hash decides at load. Auto-resolved |

`Scan` is `Unmerged` plus the duplicate-id walk; it also works on a project
with no git at all (one that arrived by rsync or a zip).

**Duplicate ids are renumbered, not guessed.** Feature ids stay sequential
because the id is the filename, and `15-extrude.json` reads well in a diff.
After a merge, `Parents` and `MergeBase` say which side added which file:
THEIR file is renumbered and ours keeps its id, and references to the old id
are rewritten only in files their side added — their extrude meant their
sketch. It refuses, naming the
referring file, when attribution is unavailable — a reference in a file both
sides modified, an id that existed in the base, no merge to attribute
against. The key scan excludes only known-foreign id spaces (`entity_id`,
`body_id`, `part_id`, …), so a new feature type over-reports rather than
under-reports. Cross-document references (a mate into a part) are out of its
scope; `Verify` is the backstop.

Sketch entity ids cannot collide this way: they are minted at random, and a
face's name is `<feature>/g<entityId>`, so with both ids unique a merged
reference cannot silently point at a different face. What a merge can still
do is combine two people's intents into a part neither designed; that is
review's job.

**`Verify` is the bar.** Before a merge is concluded every document is loaded
and REGENERATED: a resolution can parse and still dangle (their side deleted
the sketch your extrude uses). `merge_finish` refuses with `{broken}` naming
each document and what broke; "git exited 0" is never the test. Finishing
first sweeps every auto-resolvable conflict, so baked geometry's
`geometry/index.json` does not block it.

**The Conflicts panel** (`src/ui/ConflictsPanel.cpp`) exists only when there
is something to resolve: rows grouped by document, in CAD words, with Mine /
Theirs buttons labelled with the values themselves (an occurrence conflict
reads as a place: `(50, 0, 0) vs (-12.5, 0, 0) turned 90° about Z`), a
"Merge both" for auto-resolvable rows, **Finish**, and **Throw the merge
away**. Clicking a row
selects the item (see below). The scan does not run on a timer: it runs when
the disk poll sees the project move (see
[README.md](README.md#changes-made-outside-the-app)), which is the only way a
conflict can appear without the app causing it.

## Sharing through a remote

For a small team that already uses git. `GitVault` adds `Remotes`,
`AddRemote`, `RemoveRemote`, `Upstream`, `AheadBehind`, `Fetch`, `Push` and
`Clone`.

- **Authentication is git's job.** Network calls run with
  `GIT_TERMINAL_PROMPT=0` and no askpass, so a missing credential fails fast
  with git's own message instead of hanging. A team that can `git push` in a
  terminal can push here; the terminal panel is where to fix one that
  cannot.
- **Fetch, push and clone block.** In the GUI they run on a worker
  (`ui::SyncJob`, a `sync` job in the background-job registry): they touch
  only `.git/`, so the worker's whole world is a plain struct read back on
  the main thread after the join. **Merge stays on the main thread**: it
  moves the working tree under open documents. Pull is fetch, then the
  existing `Merge`.
- **A rejected push** says to get first ("pull, then push again"). The app
  never merges on the user's behalf.
- **Tags travel**: `push --follow-tags`, which is why named versions are
  annotated tags (lightweight ones are silently left behind).
- **Built geometry stops travelling.** Adding a remote
  (`ui::AddProjectRemote`) sets `commit_geometry: false` on every part and
  saves with the commit message "Stop sharing built geometry": megabytes of
  binary per save that conflict on every concurrent edit are the wrong trade
  once several people push. It first checks the project has not changed on
  disk (the save would be refused), and removes the remote again if the save
  fails, so sharing is never half-applied.
- `fetch` touches only `.git/`, which the disk stamp ignores; `pull` ends in
  `OpenProject`, which re-stamps.

The History panel's sharing section adds a remote and offers Fetch, Pull and
Push with ahead/behind counts. Clone is RPC-only (`clone_project`);
`ui::StartClone` exists but no GUI calls it.

## Seeing what changed

- **A version's changes.** Clicking a row in the History panel replaces the
  list with what changed since that version, against the working tree —
  `ProjectMerge::DiffVersions(dir, from, "")`, items added / removed /
  changed with field-level `from → to` values from the same field diff the
  Conflicts panel uses.
- **Going to a change.** Clicking a change switches to the document holding
  it and selects the item (`ui::RevealItem`); double-click also frames it,
  since a single click must not move the view under someone reading a list.
  A removed item, or a change that is not one item (an index,
  `project.json`), is disabled with a reason.
- **Compare shape** boolean-diffs the part in front of you against the same
  part at the chosen version (`ui::CompareVersionGeometry`):
  `GitVault::ExtractSubtree` writes that revision's document to a scratch
  directory file by file (falling back to the project root for a flat
  one-part project), it is loaded and regenerated, and
  `Measure::CompareBodySets` compares the body sets. The answer is `N mm³
  added / removed` or `same shape` (a millionth of the larger volume counts
  as zero), and the diff solids are drawn on the part: red where material
  went away, green where it arrived. **Hide diff** clears it.
- The overlay (`ui::SetGeometryDelta`) is tracked apart from the document
  meshes, so it is never picked, exported or saved, and it drops itself on a
  tab switch or any edit to the part — a stale picture of a part that moved
  on looks like an answer. Over RPC it is `compare_geometry
  {materialize: true}` (refused on an assembly tab; `materialized: false`
  under `--headless`) and `clear_geometry_delta`.

The version diff and Compare shape are GUI-only; there is no RPC for
`DiffVersions`.

## RPCs

All in `src/remote/RemoteProject.cpp`.

| RPC | does |
|---|---|
| `history_log {limit?}` | versions, newest first |
| `history_restore {commit}` | restore a version (committed on top) |
| `checkpoint_create {name}` | save the whole project and name the version (without git: a part-only JSON copy, `part_only: true`) |
| `checkpoint_list` | named versions `{name, ref, modified_unix}` |
| `checkpoint_restore {name}` | restore a named version, by the ref it actually has |
| `branch_list`, `branch_create {name, switch?}`, `branch_switch {name}` | local branches |
| `branch_merge {name}` | merge into the current branch; `conflicted` is an outcome, not an error |
| `conflict_list` | the scan, in document terms, duplicate ids included |
| `conflict_resolve {path, take: mine\|theirs\|auto, field?}` | `auto` is the union for an order, either side for derived geometry, a renumber for a duplicate id |
| `merge_finish` | verify every document loads and regenerates, then commit; `{broken}` otherwise |
| `merge_abort` | back to the pre-merge state |
| `remote_list` | remotes and ahead/behind |
| `remote_add {name?: origin, url}`, `remote_remove {name}` | remotes; adding one stops committing geometry |
| `fetch {remote?}`, `pull {remote?}`, `push {remote?}` | pull returns `{status, conflicts, needs_decision}`; push `rejected` means pull first |
| `clone_project {url, path}` | clone and open |

## Tests

| where | covers |
|---|---|
| `tests/test_gitvault.cpp` `[git]` | init/commit/log, tags on entries, `Dirty`, restore deleting files added after the target, branch/switch/merge, three stages, abort, delete-vs-modify, the same-id blind spot, identity resolution, remotes, rejected push, an unreachable remote failing fast, two versions sharing a name, tags reaching a remote |
| `tests/test_projectmerge.cpp` `[merge]` | the union merge (additions, deletions, insertion order, every id-keyed collection), conflicts described in CAD terms, disjoint fields never reaching the panel, duplicate-id detection, `TakeField`, the attributed renumber and its refusal, `Verify`, path summaries, occurrence conflicts reading as places |
| `tests/two_collaborators.py` | two headless instances and a bare repo, no network: push, pull, a rejected push, a resolved conflict, and the project loading and regenerating on the other side |
| GUI `history/*` | naming and restoring, rows fitting at a large font, a version's changes, clicking a change, Compare shape |
| GUI `conflicts/*` | resolving a merge in the panel, Finish refusing a broken project, a clean project not scanning |

Testing "git has no identity" needs the machine's own config hidden:
`GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null`.

The History, Conflicts and Model panels share one dock node. A GUI test
that leaves another tab selected changes the next test, so tests select
Model right after the last interaction that needed another tab (not at the
end, which a failing test never reaches).

The GUI sharing controls have no automated test; they are thin wrappers over
the RPC-tested calls.

## Not done

- **`pull` into a duplicate id** merges, then fails to reopen with the
  loader's refusal rather than reporting a conflict; `conflict_list` still
  answers, since it reads the directory.
- Opening a commit read-only, discarding uncommitted changes ("revert to last
  save"), rebase, force-push and any history rewriting.
- Locking or check-out, presence, review, a hosted service, real-time
  co-editing.
- A GUI clone dialog.
- Cross-document references in the duplicate-id renumber.
- A warning when a restore undoes someone else's commits on a shared branch.
