# F3 — Diffable directory format — design (NOT yet implemented)

> Status: design for sign-off. One genuine product decision below is
> deliberately left open — everything else is ready to build (~1–2 days).
> Gate honored: the F1 golden corpus exists and guards the migration.

## Layout

```
part.dsgn/                  # a directory IS a project
  index.json                # {format, version, rollback?, features: [ids in order]}
  parameters.json           # the variable table (absent when empty)
  features/
    1-sketch.json           # <id>-<kebab type>.json, one feature each
    2-extrude.json
```

- One file per feature, named by **stable id** (unique, never reused — the
  F1 contract) plus the type for human readability; the id alone is identity
  (renaming a feature never renames its file).
- `index.json` holds the order — the only file two feature edits both touch
  is none, so git merges of disjoint feature edits are conflict-free; a
  reorder conflicts only in the index, where it should.
- Same feature JSON dialect as today (`FeatureToJson`), same version number,
  same migrations. The single-file form REMAINS supported forever as the
  export/interchange format; `Save`/`Load` dispatch on path: `.json` file →
  single-file, directory (or `.dsgn` suffix) → directory format.
- Autosave sidecars and checkpoints keep using single-file snapshots (atomic
  rename of one file beats directory syncing for crash safety).

## Implementation sketch

- `DocumentIO::SaveDir/LoadDir` beside Save/Load, plus the dispatch;
  writes go file-per-feature via temp+rename per file, index last (a torn
  save leaves a stale-but-loadable index).
- Round-trip tests: golden corpus loaded → SaveDir → LoadDir → same geometry;
  git-friendliness test: edit one feature, assert exactly two files changed
  (feature + nothing else) — the property that justifies F3.
- RPC/GUI: `save_project`/`open_project` and the dialogs just accept
  directory paths; no new verbs.

## The open decision (for Elias)

**What is the default Save format?** Options:
1. Keep single-file default; directory is opt-in (choose a directory in the
   dialog / pass a `.dsgn` path). Safest, but git users must know to opt in.
2. Directory default for new projects; existing `.json` projects stay
   single-file until Save-As. The differentiating format becomes the normal
   one, matching the project's git-first goal — my recommendation, but it
   changes what every tutorial/screenshot shows.

F5 (git integration) works with either; it is nicer with 2.

## Why not now

Implemented late in an autonomous overnight run with the context budget
nearly spent — a format migration is exactly the wrong thing to rush. The
corpus, dispatch seam, and tests above make it a clean ~1–2 day follow-up.
