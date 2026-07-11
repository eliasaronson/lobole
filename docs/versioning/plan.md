# Versioning — file format, autosave, checkpoints, git — plan

Goals 1 + 4 of the [roadmap](../roadmap.md): named checkpoints in the model's
history, and models stored in a diffable, mergeable text format under git.
Also home to the unglamorous prerequisites: format stability and autosave —
cheap insurance that matters the moment anyone (including future-us) has real
models at stake.

## Current state

JSON project serialization, `kVersion = 9` (`src/core/Serialization.cpp`),
with load-time migrations accumulated through the sketch phases (v3→v9) and a
newer-version guard. Single monolithic file per project. No autosave, no
format spec, no git integration. Stable feature + entity ids (the hard
prerequisite for everything below) already exist.

## Milestones

### F1 — Format spec + golden-file corpus (~0.5–1 day) — [design: f1-f2.md](f1-f2.md)
Write `docs/versioning/format.md`: the JSON schema, the id-stability
guarantees, and the **migration policy** (every version bump ships a loader
migration + a round-trip test — already the de-facto practice; codify it).
Add a golden-file corpus: real saved projects, one per format version, loaded
+ regenerated + geometry-asserted in ctest. This is what makes "your old files
still open" a tested promise instead of a hope.

### F2 — Autosave + crash recovery (~0.5–1 day) — [design: f1-f2.md](f1-f2.md)
Timed autosave of dirty documents to a sidecar (`project.dsgn.autosave`),
atomic write (temp + rename), recover-on-start prompt when a sidecar is newer
than its project, sidecar cleared on clean save/exit. Dirty-state already
implied by the command stack.

### F3 — Diffable on-disk layout (~1–2 days, decide before F5)
The monolithic JSON diffs poorly and merges worse. Evaluate a **directory
format**: one file per feature (named by stable id) + an ordered index +
document-level files (parameters, settings). Feature-level files make git
diffs readable ("modified `extrude-42.json`") and most merges trivial
(different features touched → no conflict). Stable key ordering + stable ids
(both exist) are the prerequisites. Keep single-file as an export/interchange
form.

### F4 — Checkpoints + the rollback bar (~2 days)
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

### F5 — Git integration (~2–4 days)
Repo-per-project. **Shell out to `git`** rather than embedding libgit2 —
simpler, and this product's audience has git. Features, in order:
- init-on-create (optional), commit-on-save with a generated or prompted
  message; checkpoints = tags.
- History panel: log, open-read-only at a commit, restore, branch + switch.
- Merge: with F3's feature-per-file layout, git's own merge handles disjoint
  edits; a conflict falls back to "pick mine/theirs per feature" UI. A true
  semantic feature-list merge is a later, separate project — do not promise
  it in v1.

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
