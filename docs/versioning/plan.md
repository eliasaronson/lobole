# Versioning — file format, autosave, checkpoints, git — plan

> **Status 2026-08-23 — this workstream is v1-COMPLETE.** F1 format spec + golden corpus, F2 autosave/recovery,
> F3 diffable directory format, F4 checkpoints + rollback bar are shipped; F5
> shipped git as an *engine* only, and the 2026-07-14 banner here read that as
> "v1 complete" for four weeks while the app had no way to show a history.
> [F6](f6-history-ui.md) builds that half. **[F7](f7-merge-and-conflicts.md)
> shipped 2026-08-21** — branch/merge plus a Conflicts panel; scoping it found
> that the app had no verb that could *produce* a conflict, so the UI F5 and
> F6 both deferred needed a merge engine under it first. Its scoping also
> surfaced [F8](f8-external-changes.md), **also shipped 2026-08-21**: nothing
> noticed the project changing on disk, so a `git` command in the app's own
> terminal was overwritten by the next Ctrl+S. [F9](f9-collaboration.md)
> **shipped the same day**: what a small team who already use git needed on
> top — F7 had built the expensive half without meaning to. A real-GitHub
> dogfooding round then drove [F10](f10-autosave-the-project.md) (autosave
> saved the open TAB, not the project) and [F11](f11-seeing-what-changed.md)
> (sixty log rows all saying `save`), **both shipped 2026-08-22**; F11's last
> layer — the geometry delta drawn red/green ON the part — landed 2026-08-23.
> **Everything planned here is now built.** Deferred by choice, not blocked:
> **insert-feature-at-marker** (F4 v2), and the History panel's row
> truncation in the default 210 px dock. Nothing here blocks other
> workstreams. `kVersion` is now 60 (see [format.md](format.md) for the full
> history — the number has moved well past this doc's original snapshot as
> later workstreams added features/assemblies/units/CAM).

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

### F6 — Version history in the app — **shipped 2026-08-20** — [design: f6-history-ui.md](f6-history-ui.md)
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

### F8 — When the project changes underneath you — **shipped 2026-08-21** — [design: f8-external-changes.md](f8-external-changes.md)
Nothing compares the project in memory with the project on disk:
`ProjectDirty` weighs document versions, and the only filesystem comparison
anywhere is the autosave sidecar's mtime. So an external write — the terminal
panel that ships inside the app, another editor, a second instance — is
silently overwritten by the next save.

A `{fileCount, maxMtime, totalSize}` stamp of the project directory
(excluding `.git/`, which churns on its own, and `geometry/`, which F7 already
settled is derived), recorded beside `StampAllSaved` and checked at the one
place every save funnels through (`ui::SaveProject`). A save on a moved
project refuses and offers Reload or Overwrite instead of overwriting
silently. **D1 the guard, D2 the banner (focus regain plus a slow timer,
gated on git HEAD when there is one), D3 the stable-id invariant F7's
`ReplaceFeature` bug argued for — no id-0 item can reach disk.**

### F9 — A small team sharing a project — **shipped 2026-08-21** — [design: f9-collaboration.md](f9-collaboration.md)
Audience decided: a small team who **already understand git**, which is what
makes this a week rather than a quarter. F7 built the expensive half by
accident — `pull` is `fetch` + `merge`, and merge already understands the
format, so a teammate's incoming work is the same five conflict classes, the
same panel, the same integrity pass.

What remains: **C1** commits authored by the person who made them (the inline
identity is applied on every commit, so it overrides the one git already has);
**C2** remotes + clone, whose substance is that fetch/push are network calls
and `RunGit` blocks — fetch and push go to a worker thread (they touch `.git/`
only), merge stays on the main thread (it moves the working tree under an open
document); **C3** duplicate ids attributed by side, which keeps ids sequential
by using the merge base to know whose reference meant which; **C4** built
geometry stops travelling once a project has a remote; **C5** named versions
pushed, with the address disambiguated on collision rather than the name.
Auth is deliberately not handled — the audience has git working, and a prompt
that would hang is made to fail instead. Verified end to end without a
network by `tests/two_collaborators.py`: a bare repo, two clones, two headless
instances, a rejected push, a resolved conflict, and the project loading and
regenerating on the other person's side.

**A handoff for whoever picks this up is in
[f7-handoff.md](f7-handoff.md)** — what is built, what deliberately is not,
which tests cover what, and the traps.

### F10 — Autosave saves the project, not the open tab — **shipped 2026-08-22** — [design: f10-autosave-the-project.md](f10-autosave-the-project.md)
**A bug, not a feature.** F6 found `checkpoint_create` snapshotting the ACTIVE
PART and called it "wrong, not merely old"; autosave was never revisited and
still does exactly that (`Autosaver::MaybeSave` takes one `Document`).
Demonstrated: on a two-part project the sidecar holds only the open tab's
features, and **on an assembly or drawing tab nothing autosaves at all**
(`state.document` is null there). Recovery opens the sidecar as a project, so
accepting it on a 48-part project would replace it with one part — the
recovery is more destructive than the crash.

The decision it needs is real: F2 chose a single-file sidecar because "atomic
rename of one file beats directory syncing for crash safety", and a tool whose
job is surviving a crash must never offer a half-written recovery. The answer
is **one atomic file per dirty document** under a sidecar directory — every
file stays complete-or-absent, unchanged documents stop being rewritten, and
no new format is invented. **~1 day.**

### F11 — Seeing what changed — **shipped 2026-08-22, layer 4 on 2026-08-23** — [design: f11-seeing-what-changed.md](f11-seeing-what-changed.md)
The version system works and is invisible: after a day the panel is sixty rows
saying `save`, and F9 sharpened it — pull a teammate's work and there is no way
to see what arrived. **G1** gives every row what it touched, in the tabs' own
words, by lifting the path→CAD-language translation ProjectMerge already has
(paths for most rows, one blob read to name a single item). **G2** is a diff
between two versions, where the field-level layer is `ProjectMerge::DiffFields`
already written — and where `compare_geometry` genuinely fits, since a diff has
two operands by construction and a merge does not. **G3** is an hour of
cleanup: `ProjectMerge::Scan` walks the tree and spawns `git` every second on a
clean project. **G3, G1, G2 — all shipped**, in that order. G3 is measured: 0 scans over 60
clean frames, where it had been one per second. Clicking a version shows what
changed since it, against the working tree, so the everyday question needs no
second pick. **Layer 3** boolean-diffs the part against a version
(`CompareVersionGeometry`), and **layer 4** (2026-08-23) draws that diff *on*
the part — red where material went away, green where it arrived — which is
V4's `materialize`, finally built. A volume answers "how much"; only a shape
answers "where".

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
- The remaining gap F7's scoping found is now [F8](f8-external-changes.md).
  The other one still stands: the versioning docs are written as if a project
  were a single part, and a conflict spans every document in it.
- **Toponaming divergence** was recorded here as F9's unfixable risk. It was
  narrower than that: a face name is `<feature>/g<entityId>`, so it can only
  mislead when one of those ids means two things — feature ids (C3) or sketch
  entity ids, which were a per-sketch counter until C6 made them
  collision-resistant and made a duplicate a load-time refusal. What remains
  is that a merge combines two people's *intents*, which is review's job, not
  a checker's.
