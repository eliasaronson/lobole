# Versioning — file format, autosave, checkpoints, git — plan

> **Status 2026-08-20.** F1 format spec + golden corpus, F2 autosave/recovery,
> F3 diffable directory format, F4 checkpoints + rollback bar are shipped; F5
> shipped git as an *engine* only, and the 2026-07-14 banner here read that as
> "v1 complete" for four weeks while the app had no way to show a history.
> [F6](f6-history-ui.md) builds that half. Later work: **merge and conflict
> handling** — scoped 2026-08-21 as [F7](f7-merge-and-conflicts.md), which
> found that the app has no verb that can *produce* a conflict, so the UI F5
> and F6 both deferred needs a merge engine under it first — and
> **insert-feature-at-marker** (F4 v2). Nothing here blocks other
> workstreams. `kVersion` is now 28 (see [format.md](format.md) for the full
> history — the number has moved well past this doc's original snapshot as
> later workstreams added features/assemblies/units).

Goals 1 + 4 of the [roadmap](../roadmap.md): named checkpoints in the model's
history, and models stored in a diffable, mergeable text format under git.
Also home to the unglamorous prerequisites: format stability and autosave —
cheap insurance that matters the moment anyone (including future-us) has real
models at stake.

## Current state (original snapshot; see the status banner above for what shipped)

JSON project serialization, `kVersion = 18` (`src/core/Serialization.cpp`),
with load-time migrations back to v1, a newer-version guard, a written
contract + migration policy ([format.md](format.md)), a golden-file corpus
(`tests/golden/`, enforced by `test_golden.cpp`), and autosave with crash
recovery (`core/Autosaver`, GUI modal + `open_project.autosave_available`
over RPC). Single monolithic file per project; no git integration yet.
Stable feature + entity ids already exist.

## Milestones

### F1 — Format spec + golden-file corpus — **shipped (July 2026)** — [design: f1-f2.md](f1-f2.md)
Write `docs/versioning/format.md`: the JSON schema, the id-stability
guarantees, and the **migration policy** (every version bump ships a loader
migration + a round-trip test — already the de-facto practice; codify it).
Add a golden-file corpus: real saved projects, one per format version, loaded
+ regenerated + geometry-asserted in ctest. This is what makes "your old files
still open" a tested promise instead of a hope.

### F2 — Autosave + crash recovery — **shipped (July 2026)** — [design: f1-f2.md](f1-f2.md)
Timed autosave of dirty documents to a sidecar (`project.dsgn.autosave`),
atomic write (temp + rename), recover-on-start prompt when a sidecar is newer
than its project, sidecar cleared on clean save/exit. Dirty-state already
implied by the command stack.

### F3 — Diffable on-disk layout — **shipped (July 2026)** — [design: f3-directory-format.md](f3-directory-format.md)
The monolithic JSON diffs poorly and merges worse. Evaluate a **directory
format**: one file per feature (named by stable id) + an ordered index +
document-level files (parameters, settings). Feature-level files make git
diffs readable ("modified `extrude-42.json`") and most merges trivial
(different features touched → no conflict). Stable key ordering + stable ids
(both exist) are the prerequisites. Keep single-file as an export/interchange
form.

### F4 — Checkpoints + the rollback bar — **shipped (July 2026)** (rollback bar via the regeneration track; named checkpoints v1 under `<project>.checkpoints/`; insert-at-marker still v2)
Two related things, built together:
- **Rollback bar** (SolidWorks-style): a marker in the feature list;
  regeneration stops there, features after it show grayed. Implementation is
  small (`Regenerate(upTo)` + history-panel UI) and it doubles as the
  inspection tool for checkpoint diffs. Insert-feature-at-marker comes free
  conceptually but needs downstream-reference validation — v2.
- **Named checkpoints**: a label on a document state. With git (F5) a
  checkpoint *is* a commit/tag created through the UI; before F5 it's a saved
  copy under `.checkpoints/`. Checkpoint list panel: view (read-only open),
  roll back to, branch from.

### F5 — Git integration — **engine shipped (July 2026, v1: linear history); UI in [F6](f6-history-ui.md)**
Repo-per-project. **Shell out to `git`** rather than embedding libgit2 —
simpler, and this product's audience has git. Features, in order:
- init-on-create (optional), commit-on-save with a generated or prompted
  message; checkpoints = tags. — **shipped**, except that the *prompted*
  message and the GUI half of "checkpoints = tags" were not: the File menu
  still wrote F4's part-only JSON copies. Closed by F6.
- History panel: log, open-read-only at a commit, restore, branch + switch.
  — **not built in F5.** `GitVault::Log`/`RestoreTo` exist and are reachable
  over RPC, but `src/ui/` called only `CommitAll`. F6 builds the panel with
  log + restore; open-read-only and branch/switch are deferred there, with
  reasons.
- Merge: with F3's feature-per-file layout, git's own merge handles disjoint
  edits; a conflict falls back to "pick mine/theirs per feature" UI. A true
  semantic feature-list merge is a later, separate project — do not promise
  it in v1. — **the conflict UI was not built**, and this bullet understated
  it twice over: there is no branch/merge/remote either, so the app cannot
  reach the state the UI would serve, and "pick per feature" covers only one
  of the five conflict classes the format actually has. Scoped as
  [F7](f7-merge-and-conflicts.md). The 2026-08-19 merge-safety work
  (duplicate-id refusal, named parse errors) remains the current answer:
  refuse loudly rather than resolve quietly.

### F6 — Version history in the app — [design: f6-history-ui.md](f6-history-ui.md)
The user-visible half of F5: a History panel (named versions, restore),
checkpoints as project-wide named commits, and the `RestoreTo` fix — it used
`git checkout <commit> -- .`, which leaves files added *after* the target
commit in place, so restoring past a part's creation kept the part.

### F7 — Merge + conflict handling — **shipped 2026-08-21 (A+B+C, local branches only)** — [design: f7-merge-and-conflicts.md](f7-merge-and-conflicts.md)
What F5 and F6 each deferred in a sentence, costed. The finding that reorders
it: `GitVault` is linear-only (no branch, merge, remote or fetch), so a
conflict can only arrive from a terminal or a sync folder — the resolution UI
is the *last* 20% of a collaboration workstream, not a follow-on to the
history panel. Phases: A make a conflict reachable (~1–2 d), B teach the app
what a conflict means in CAD terms (~2–4 d, the real work — the three stages
parsed as features, a union merge for `index.json` and the eight id-keyed
collections, and a reference-integrity pass before the merge commit), C the
Conflicts panel (~2–3 d). **5–9 days without remotes.**

Two decisions the build settled:
- **Feature ids stay sequential.** Random 64-bit ids would delete the
  duplicate class, but the id IS the filename and F3 chose that for
  readability. `RenumberDuplicate` handles the class instead — and *refuses*
  rather than guessing when anything still points at the contested id.
- **The panel exists only when there is something to resolve.** No format
  change; `kVersion` does not move (git objects are not the document format).

## Recommended order

F1 + F2 **soon** (each ~a day, pure insurance). F3 after the schema settles —
concretely, after variables (new document-level param table) and the first
modeling milestones (new feature kinds), so the layout isn't designed against
a moving target. F4 → F5 after that; F4's rollback bar is independently
useful and could be pulled forward with the modeling work.

## Risks

- F3's format change is the riskiest migration so far — gate it on F1's golden
  corpus existing first.
- Checkpoint/branch UX can balloon; keep F4/F5 v1 to list + view + restore +
  branch, nothing more.
- F7's risk is the opposite one: it is easy to build a Conflicts panel that
  looks finished while resolving only same-field feature edits, and silently
  drops work on an `index.json` pick or a duplicate id. Its acceptance bar is
  "the resolved project loads and regenerates", never "git exited 0".
- Two gaps F7's scoping found that are live *now*, independent of it: nothing
  detects the project directory changing on disk (so a `git` command run in
  the app's own terminal panel is clobbered by the next Ctrl+S), and the
  versioning docs are written as if a project were a single part.
