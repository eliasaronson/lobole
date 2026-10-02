# Saved study results at a sensible size

## Problem

Since v153 a project saves every study's result next to it, so a reopened
project reuses results instead of re-running them
(docs/simulation/README.md#studies-as-jobs). The files are far larger than
they need to be. The pan/tilt yoke's envelope project is 237 MB, of which the
yoke's static result is 52 MB, and the pan/tilt scratch tree is 468 MB. Every
save rewrites them, and so does every autosave of a dirty document. A
versioned project (GitVault) stores each version: a few edits and a re-run,
and the history holds gigabytes.

## Fix

**Measure first.** For each result kind (static, modal, motion, topology),
on the yoke, list what the file holds, byte by byte by field. Then cut:

- **Only what is read.** Keep a field only if a panel or an RPC reads it back
  from a saved result. Leave out anything rebuilt from the mesh, from the
  model, or from other fields (derived stresses, per-element duplicates of
  per-node data). Store the mesh once per study, not once per load case or
  mode.
- **Binary, not JSON text.** Write each numeric array as raw little-endian
  floats in a sidecar `.bin` next to the result's `.json`, which keeps the
  metadata and the `input_hash`. Results are derived bulk data, so float is
  right (docs/conventions.md#the-model-is-double). Use zstd if the build
  already links it; otherwise leave it out.
- **Written once.** A save whose result has not changed since the last save
  does not rewrite it. An autosave never writes results.
- **The version history.** A result is reproducible from its inputs, so
  decide whether GitVault stores result files at all, or keeps only the
  newest. State the choice in docs/versioning.

**Target:** the yoke's projects at least 10× smaller, and a reopen still
reusing every result.

## Files

- the result serialization in `src/core/fem/` and `src/core/MotionStudy.*`;
- the project save path and the autosaver;
- GitVault's file list;
- `docs/versioning/format.md` (a format change).

## Verification

- Sizes before and after on the pan/tilt projects.
- Every kind reopens current with identical data. The existing reopen tests
  and `study_reopen_smoke.py` pass unchanged.
- A save with nothing changed leaves the result files' modification times
  alone.
- An autosave writes no result.
