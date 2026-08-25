# F7 — Merge and conflict handling — design

> **SHIPPED 2026-08-21** — phases A, B and C. This began as the scoping
> [F5](plan.md#f5--git-integration) and [F6](f6-history-ui.md) both deferred
> in one sentence; the sections below are the design as written, with a
> "What shipped" section at the end recording where building it changed the
> plan. Local branches only — remotes are still out.

## Why this exists

F5 said it in one line: *"with F3's feature-per-file layout, git's own merge
handles disjoint edits; a conflict falls back to 'pick mine/theirs per
feature' UI."* F6 repeated it as a deferral. Both sentences assume a merge
happens.

**The app cannot produce a conflict.** `GitVault` is 208 lines and eight
functions — `Available`, `IsRepo`, `Init`, `Dirty`, `CommitAll`, `Tag`,
`Log`, `RestoreTo` — all local and strictly linear. There is no branch, no
merge, no remote, no fetch. A conflicted `.dsgn` can only arrive from a
terminal, a sync folder, or a second clone.

So the conflict UI is not the tail of F6. It is the tail of a collaboration
workstream that was never planned, and it has to be built in that order: a
way to get two histories, a way to bring them together, and *then* a way to
resolve what collides. Building the UI first produces a screen for a state
the app has no verb to reach.

The current answer — refuse loudly — is genuinely the right v0 and is
already in place: `ParseProjectFile` names the file and recognises
`<<<<<<<` markers (`Serialization.cpp:445`), `InsertIdFile` refuses two
files claiming one id (`:477`), `CheckIndexIdsUnique` refuses an index that
lists an id twice (`:493`). F7 is what replaces refusal with resolution, and
it should not start until refusal is actually costing something.

## What a conflict *is* here

"Pick mine or theirs per feature" describes one of five classes, and it is
the rare one. The others need different machinery, and two of them are not
picks at all.

**1. One feature file, both sides edited.** The mine/theirs pick F5
described. Note what the 2026-08-19 merge audit measured: git already merges
*disjoint fields* of one feature correctly (Alice edits `distance`, Bob
renames it, both survive) because the JSON is one key per line, sorted. So a
genuinely conflicted feature file means the two sides touched the *same
field* — the narrow case. This is the easy fifth of the work.

**2. `index.json`.** F3 says a reorder "conflicts only in the index, where it
should" and stops there. It never says what resolves it. Mine/theirs on the
index is **lossy in a way mine/theirs on a feature is not**: taking your side
drops the other side's new features out of the order while their files sit on
disk — they either vanish from the document or the load refuses. The index
needs a real three-way union merge anchored on the merge base, not a button.
The same is true of every ordered id list in the format.

**3. Duplicate ids — invisible to git.** Ids come from `nextId++`, so two
people offline both mint feature 15. When the type suffix differs, git keeps
`15-box.json` *and* `15-hole.json` with **no conflict at all**: two new
files, no overlap, a clean merge. A conflict UI driven off
`git diff --diff-filter=U` will show nothing, and this is the one failure
mode that has actually eaten work (a project that opened as
`['Bob box', 'Bob box']` with the other feature silently gone, refused since
0363a3e). Resolving it means renumbering one side *and every reference to
it* — `createdBy`, toponaming refs, mates, drawing dimensions, config
overrides. A graph rewrite, not a pick. **See the prerequisite below: this
class can be deleted instead of handled.**

**4. The other id-keyed collections.** The same union-merge problem as the
index, eight times over: `feature`, `study`, `study result`, `configuration`,
`occurrence`, `mate`, `view`, `dimension`. A part conflict and an assembly
conflict are the same shape.

**5. `geometry/*.brep` — free.** Already `binary -merge -diff` in
`.gitattributes` (`GeometryCache.cpp:282`) and keyed by the chain hash, so
either side can be taken and rebuilt. Worth stating explicitly so nobody
spends a day on it.

## The id decision (settled 2026-08-21: keep them sequential)

Random 64-bit ids instead of `nextId++` would **delete class 3 entirely** —
two offline sides could no longer mint the same id. The first draft of this
doc called that a prerequisite. On a closer look the trade is not one-sided:
**the id IS the filename**, and F3 chose `15-extrude.json` so that a git diff
reads as "modified extrude 15". Opaque `7f3a9c2b1d4e5f60-extrude.json` names
give that up, and every test, script and golden that assumes ids 1, 2, 3
regenerates.

Decision (Elias): keep sequential ids and handle the duplicate class in F7.
`RenumberDuplicate` is what that costs, and it turned out cheaper than
expected because it **refuses instead of guessing** (below).

## Phases

### A — make a conflict reachable (~1–2 days)

`GitVault` grows, in the same shell-out style: `Branch`/`Switch`/`Branches`,
`Merge`, `Unmerged()` (`git diff --name-only --diff-filter=U`),
`ResolveWith(path, ours|theirs)`, `AbortMerge`, `CommitMerge`. About 200
lines; `tests/test_gitvault.cpp` already builds scratch repos and scripts
divergence, so the tests are cheap.

**Remotes stay out of v1.** Push/pull drags in SSH keys, HTTPS tokens,
host-key prompts and their whole error surface. Local branches plus a second
copy of the folder covers the real case — one person, two machines — and is
testable without a network.

### B — what a conflict means in CAD terms (~2–4 days; the actual work)

`features/15-extrude.json is conflicted` is not a UI. The three stages come
out of git (`git show :1:path`, `:2:`, `:3:`) and through `FeatureFromJson`,
and a field diff of the parsed JSON turns them into a sentence a person can
act on: *"Extrude 'Boss' — distance 12 mm (yours) vs 18 mm (theirs)"*. On top
of that: the union merge for classes 2 and 4, and a **validation pass before
the merge commit** — a resolution can parse and still leave a dangling
reference (theirs deleted the sketch yours extrudes). "Does it parse" is the
only bar today. The acceptance test is that the resolved tree loads and
regenerates, not that it merges.

### C — the GUI (~2–3 days)

A Conflicts tab in the History panel's mold (`HistoryPanel.cpp`, 351 lines —
the same panel shape, the same one-line refusal when git is missing). Rows
grouped by document, in CAD language, each with Mine / Theirs; abort leaves
the project exactly as it was. Two entry points: an in-app merge, and
**opening a project that is already mid-merge** (`MERGE_HEAD` or unmerged
paths present), which opens read-only into this view. `OpenProject` is
already the reload primitive — `RestoreVersion` uses it the same way.

**Total: 5–9 focused days** for an honest v1 without remotes. With push/pull
and credentials, 3–5 more, most of it auth misery rather than CAD.

## The cheaper thing to build first (~1 day)

**Triage, not resolution.** Detect a conflicted or duplicate-id project at
open — the refusals already know — and instead of one error line, show the
list in CAD language ("Extrude 'Boss' in part Gangway"), offer whole-file
mine/theirs where that is safe, and a button that hands the mess to the
terminal.

That last part is nearly free: the terminal panel already ships, and it
already starts in the open project's directory
(`TerminalPanel.cpp:116-130`). The user who hit a conflict on their own two
machines gets a real way through it without the app owning a merge engine.

This is the recommended first move. It is the honest version of "refuse
loudly" — refuse *usefully* — and if it turns out to be enough, phases A–C
never need to happen.

## Adjacent gaps this scoping surfaced

These are real today and independent of whether F7 is ever built.

- **Nothing detects the project changing on disk.** Only the autosave sidecar
  compares mtimes (`Autosaver.cpp:33`). Now that a terminal runs *inside* the
  app, a `git merge` or `git checkout` there happens while the in-memory
  document is authoritative, and the next Ctrl+S clobbers it. This is a live
  trap, not a hypothetical one, and it arrived with the terminal panel.
  **Planned as [F8](f8-external-changes.md)** — and note what F7 does NOT
  make worse: switch, merge and restore each reopen the project afterwards,
  which is the discipline to keep.
- **The versioning docs are written as if a project is one part.** A conflict
  spans 48 parts plus the assembly plus the drawings. Which tab opens? What
  does a conflict in the assembly *and* a part look like in one list? F6 hit
  the same assumption from the other side (the part-only checkpoint bug).
- **Autosave sidecar vs a mid-merge tree** is undefined: recovery could offer
  to restore over a half-resolved merge.
- **No visual diff.** "Show me what changed between Tuesday and now" as
  geometry rather than JSON is absent from every phase of the versioning
  plan, and is plausibly worth more than conflict resolution — it serves the
  solo user, who is the only user there is.

## Decisions (settled 2026-08-21)

1. **The full engine**, not triage-only — phases A–C are what roadmap goal 4's
   "mergeable text format" actually promises.
2. **Local branches only** here; remotes followed the next day as
   [F9](f9-collaboration.md), where auth could be scoped deliberately.
3. **Feature ids stayed sequential** — the id IS the filename, and F3 chose
   that for readable diffs. F9 C3 handles the duplicate class by attributing
   it to a merge side instead. (Sketch ENTITY ids are a different story and
   did change: see [F9's C6](f9-collaboration.md), where the name is never a
   filename and the argument does not apply.)

## What shipped, and what building it changed

Phases A, B and C, 2026-08-21. `GitVault` gained branch/switch/merge,
`Unmerged`, three-stage reads, `ResolveWith`/`AbortMerge`/`CommitMerge`;
`core/io/ProjectMerge.{h,cpp}` is the CAD-level model; `ui/ConflictsPanel`
is the panel; ten RPCs (`branch_*`, `conflict_*`, `merge_*`). 35 new
assertions in `test_gitvault.cpp`, 19 cases in `test_projectmerge.cpp`, two
`conflicts/*` GUI tests.

Six things the build changed about the plan above.

**The panel only exists when there is something to resolve.** The first
sketch had it as a permanent tab beside History. A tab that says "no
conflicts" every day of the year is noise, and this is the one panel whose
*absence* is the good news, so `DrawConflictsPanel` returns before `Begin`
when the scan is clean. Its appearance is the notification.

**"Finish" has to sweep the auto-resolvable conflicts itself.** Caught by
the GUI test, not by reasoning: any project with baked geometry conflicts on
`geometry/index.json` (only `*.brep` is marked binary, not the index beside
them), so `CommitMerge` refused on an unmerged path even though the panel
showed nothing left to decide. `FinishMerge` now resolves every
`autoResolvable` conflict before verifying. They stay listed with a button so
nothing is silent — but nobody clicks "Merge both" once per index.

**Renumbering refuses instead of guessing, and that is the feature.** The
plan described a graph rewrite: remint an id, then chase every reference
(`createdBy`, toponaming, mates, drawing dims, config overrides). There is no
reference visitor to hang that on, and a hand-maintained key table would rot
into silent breakage. Instead `RenumberDuplicate` scans for anything still
pointing at the contested id and **refuses when it finds one**, naming the
file — because a `sketch_id: 15` genuinely cannot say which of two features
it meant, and picking one would quietly re-parent someone's geometry. The
common case (both sides appended a new feature nothing references yet)
resolves automatically; the ambiguous case is reported. The key list is
inverted for the same reason: only *known-foreign* id spaces (`entity_id`,
`body_id`, `part_id`, …) are excluded, so a feature type added later
over-reports rather than under-reports.

**Ordering in the union merge is not "append".** Both sides appending after
the same entry produced `[1,2,20,10]` — their addition in front of ours.
Their additions now anchor to the entry that preceded them *on their side*
and then step over ours. Order is not cosmetic in a feature list: an extrude
regenerates after the sketch it uses.

**A bug under the whole thing: `modify_feature` destroyed stable ids.**
Found by driving the new RPCs end to end, not by any test.
`Document::ReplaceFeature` swapped the incoming feature in wholesale, and the
`modify_feature` RPC sends parameters with no `"id"` — so every RPC-driven
edit silently reset the feature's identity to 0. On disk that is
`features/0-box.json` with `"features": [0]` in the index, and every
reference that named the old id (`sketch_id`, toponaming, mates, drawing
dimensions) dangles. Stable, never-reused ids are the F1 contract that F3's
one-file-per-id layout and every line of F7 rest on, so this was sand under
the foundation. `ReplaceFeature` now inherits the old id when the
replacement carries none, and keeps an explicit one when it does.

`headless_smoke` had encoded the old behaviour — it manufactured a dangling
datum reference by omitting the id, with a comment claiming that produced "a
fresh feature (new id)". It produced id 0. The test now names a different id
explicitly, which is the honest way to say "this is a different feature".

**The panel's language cost a rename.** `history/rows_fit_at_a_large_font`
(F6's guard) failed the moment the branch section landed: the collapsed
TreeNode label "Separate lines of work" was 234 px against a 218 px panel at
1.4× font. It is "Lines of work" now, the status line wraps, and the two
row buttons go through the panel's existing measured-fit helper. The F6
lesson held exactly as written — and the guard earned its keep on the first
new section added since.

### Still not done

- **Remotes.** Deliberate. Local branches plus a second clone of the folder
  covers one person with two machines; push/pull is auth misery.
- **Cross-document references** in the duplicate-id scan (an assembly mate
  into a part). Id spaces are per-document, so scanning the whole project
  would report collisions that are not references. `Verify` is the backstop.
- The **disk-change gap** below was open and live when this was written;
  **[F8](f8-external-changes.md) closed it on 2026-08-21** (a project-directory
  stamp checked at the one place every save funnels through), together with the
  stable-id invariant that the `ReplaceFeature` bug argues for. Remotes were
  deferred here too and **F9 shipped them the same day**.

## Verification sketch

- Unit (`test_gitvault.cpp`): branch, diverge, merge, assert `Unmerged()`
  names the expected files; `ResolveWith` + `CommitMerge` leaves a clean tree;
  `AbortMerge` restores the pre-merge state exactly.
- Unit (serialization): two scratch projects diverge on the *same field* of
  one feature, on the index order, and on ids; each resolution **loads and
  regenerates to the expected geometry**. This is the bar, not "git exited 0".
- GUI (`conflicts/*`): open a mid-merge project → the panel appears with the
  right rows; picking resolves and reopens; abort leaves the project as found.
