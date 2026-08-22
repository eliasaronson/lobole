# F10 — Autosave saves the project, not the open tab — design

> **Status: not built.** Planned 2026-08-22. This is a **bug fix**, not a
> feature: the crash-safety mechanism does not currently protect a
> multi-document project, and on an assembly or drawing tab it does not run at
> all.

## The bug

[F6](f6-history-ui.md) found that `checkpoint_create` snapshotted
`*state.document` — the *active part* — so "create checkpoint" on the dock
project saved whichever tab happened to be open and silently omitted the
assembly, the other 48 parts and the drawings. F6 called that "wrong, not
merely old" and fixed it by routing checkpoints through `ui::SaveProject`.

**Autosave was never revisited.** `Autosaver::MaybeSave` takes a single
`Document`, and `main.cpp:1627` hands it `g_App.document`.

Demonstrated on a two-part project, autosave interval 1 s:

```
project documents: ['Part 1', 'Second']
sidecar holds features: ['PartTwoBody']
  -> PartOne's work present? False
sidecar recreated while on an assembly tab: False
```

Two failures, and the second is worse:

1. **A multi-document project autosaves only the active part.** Recovery opens
   the sidecar *as a project* (`FileDialogs.cpp:129`), so accepting a crash
   recovery on a 48-part project would replace it with one part. The crash
   recovery is more destructive than the crash.
2. **On an assembly or drawing tab, nothing autosaves at all.**
   `state.document` is null there, and the `if (g_App.document)` guard skips
   silently. An hour of assembly work, a crash, nothing kept.

## The decision this needs, and why it is not obvious

[F2](f1-f2.md) chose a **single-file** sidecar (`project.dsgn.autosave`) for a
stated reason, repeated in [F3](f3-directory-format.md): *"atomic rename of one
file beats directory syncing for crash safety."* That reasoning is still
right. A tool whose job is surviving a crash must never leave a half-written
recovery file, because the recovery prompt would then offer garbage to someone
who has already lost their work once.

So "just save the whole project to the sidecar" is not free — `SaveProject`
writes a directory tree, and a crash halfway through leaves a partial one.

Three options:

**A. A sidecar directory written like a project.** Simplest to write, and it
loses exactly the property F2 chose. Atomicity could be recovered by writing to
a temp directory and renaming — but renaming a directory over an existing one
is not a single operation, so there is still a window. Rejected.

**B. Invent a single-file form for a whole project.** Keeps one atomic write,
but a multi-document project has no single-file representation today, so this
means a new (if transient) shape to write, read and maintain. Rejected as
disproportionate.

**C. One atomic file per document.** `project.dsgn.autosave/<document>.json`,
each written temp+rename exactly as today, and **only for documents that are
dirty**. Recommended.

C keeps F2's guarantee intact where it matters — every individual sidecar file
is complete or absent, never torn — because the directory holding them needs
no atomicity of its own. It also stops rewriting 48 unchanged parts every
60 seconds, which the current design would have done at project scale. And it
needs no new format: each file is the existing single-document form.

## What changes

- **`Autosaver` takes the project, not a document.** Its dirty-tracking is
  per document today (`m_lastSavedDocInstance` / `m_lastSavedDocVersion`, one
  pair); it becomes a map keyed by the document's stable id. That map is the
  substance of the change — the rest is a loop.
- **Assemblies and drawings autosave too**, through `SaveAssembly` /
  `SaveDrawing`, which already exist. This is what closes failure 2.
- **`HasNewerAutosave` becomes "which documents have one"**, and the recovery
  modal says how many rather than offering a single file: *"3 documents have
  unsaved changes from a previous session."*
- **The sidecar directory is ignored by git.** It sits beside the project
  today (`projectPath + ".autosave"`), outside the repository, and a directory
  in the same place stays outside it too — but F8's stamp deliberately walks
  only the project directory, so nothing here disturbs it either.

## Open decisions (for Elias)

1. **Sibling directory, or the state dir?** Sibling (`project.dsgn.autosave/`)
   keeps F2's reasoning — it is obvious, and it travels with the project if
   someone moves the folder. The state dir keeps the project folder clean.
   I recommend sibling, for continuity.
2. **Is recovery all-or-nothing?** Recovering some documents and not others
   leaves a project half from one session and half from another, which is a
   state nobody can reason about. I recommend all-or-nothing for v1, with the
   count shown so the choice is informed.
3. **Should a clean save clear every sidecar, or only the saved document's?**
   Today's `Autosaver::Clear(path)` is whole-project. Keeping that is simplest
   and matches "a save means the disk is now the truth".

## Verification

- Unit: a two-document project where only one is dirty writes one sidecar;
  dirtying the other adds a second; a clean save clears both.
- Unit: an assembly document autosaves (the case that produced nothing at
  all).
- Unit: each sidecar is written temp+rename — the property F2 chose, asserted
  rather than assumed.
- Headless: build a multi-part project, dirty two documents, force autosave,
  kill the process, reopen — **the recovery offer names both, and accepting it
  restores both**. That is the bar, and it is the scenario the current code
  fails.
- The existing `headless_smoke` autosave assertions keep passing unchanged.

## Effort

**~1 day.** The loop and the assembly/drawing serializers are mechanical; the
per-document dirty tracking and the recovery prompt are the real work.
