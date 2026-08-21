# F7 — Merge and conflict handling — design

> **Status: not built.** This is the scoping [F5](plan.md#f5--git-integration)
> and [F6](f6-history-ui.md) both deferred in one sentence. Written
> 2026-08-21 against the shipped engine; nothing here is promised for a date.

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

## Prerequisite: non-sequential ids

Random 64-bit ids instead of `nextId++` **delete class 3 entirely** — two
offline sides can no longer mint the same id, so the collision that git
cannot see stops existing. It is a format change wanting a `kVersion` bump
and a golden pair, so roughly half a day, and it is filed today as a
"still open, when two people actually share a project" nicety. It is not a
nicety; it is cheaper than the code that would otherwise have to handle its
consequences, and it should land before any of the phases below.

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

## Open decisions (for Elias)

1. **Triage-only, or the full engine?** The recommendation above is triage
   first. The counter-argument is that phases A–C are what "mergeable text
   format" (roadmap goal 4) actually promises, and triage does not deliver
   it.
2. **Local branches only, or remotes?** v1 above says local. Remotes are what
   makes it collaboration rather than a two-machine convenience.
3. **Do the id change now, regardless?** It is half a day, it is a `kVersion`
   bump, and it removes a class of silent data loss whether or not F7 is
   ever built.

## Verification sketch

- Unit (`test_gitvault.cpp`): branch, diverge, merge, assert `Unmerged()`
  names the expected files; `ResolveWith` + `CommitMerge` leaves a clean tree;
  `AbortMerge` restores the pre-merge state exactly.
- Unit (serialization): two scratch projects diverge on the *same field* of
  one feature, on the index order, and on ids; each resolution **loads and
  regenerates to the expected geometry**. This is the bar, not "git exited 0".
- GUI (`conflicts/*`): open a mid-merge project → the panel appears with the
  right rows; picking resolves and reopens; abort leaves the project as found.
