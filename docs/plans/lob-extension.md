# `.lob` is the project extension

## Problem

A project folder is named `<name>.dsgn`, after the app's old name. The
extension becomes `.lob`. 0.1.5 is released and writes `.dsgn`, and the
scratch tree is full of them, so a `.dsgn` must keep OPENING — but the app
must never write a new one.

## Shape

**`.lob` is the folder form only; `.json` stays the single-file form.** A
single-file `.lob` was considered and is not clean:

- A path that does not exist yet cannot say which form it wants. Today the
  extension decides (`DocumentIO::IsDirFormatPath`: `.dsgn` or an existing
  directory is the folder form; `ProjectSaveTarget` turns any GUI name into a
  `.dsgn`). A `.lob` that may be either needs a second input on every writer,
  or a rule ("a new `.lob` is a folder") under which the app never writes
  the file form at all.
- The two forms differ in capability: the folder has git history, adopted
  import sidecars and baked geometry; the file has none. `SaveProject`
  already refuses a multi-document save to `.json` because a directory named
  `w170s.json` once silently lost its history — "same format, different
  capability, decided by a filename". One extension for both brings that back.
- The reason to want it — a double-click in a file manager — does not reach
  the folder form anyway: GIO types every directory `inode/directory`
  whatever its name (`gio info -a standard::content-type x.lob` on a folder),
  so no MIME glob can match a project folder.

So: `.lob` replaces `.dsgn` wherever the code means "the folder form";
`.json` is unchanged.

**One place recognises a project path.** `ui::IsProjectPath` /
`ui::NormalizeProjectPick` (ProjectActions.cpp) accept `.lob`, `.dsgn`, a
`.json`, or a directory. In core, `IsDirFormatPath` / `NormalizeProjectPath`
(Serialization.cpp) take both extensions; `ProjectSaveTarget` maps a picked
name to `.lob` (a typed `.dsgn` or `.json` is replaced). Every
`ends_with(".dsgn")` (git history in `MarkProjectSaved`, `CanVersionProject`,
import adoption in `SaveProject`) asks one helper instead,
`DocumentIO::IsProjectFolderName`.

**Saving a `.dsgn` writes a `.lob` beside it.** In `ui::SaveProject`, the one
funnel every save goes through, a target ending `.dsgn` is mapped to the
sibling `.lob` (`ui::ProjectWriteTarget`):

- Only the open project's own path migrates (Ctrl+S, naming a version,
  `save_project` with that path). Any other `.dsgn` target is refused, naming
  the `.lob` to use — a new `.dsgn` is never written.
- If the `.lob` already exists the save is refused: it is another project
  (or an earlier migration) and saving over it would lose it.
- Otherwise the whole `.dsgn` folder is COPIED to the `.lob` first — git
  history, import sidecars, assets, baked geometry — and the save then writes
  over the copy and commits there. Copy, not move: moving `.git` out would
  leave the `.dsgn` a project without its history, and the user's `.dsgn` is
  never changed or deleted.
- `MarkProjectSaved` applies the same mapping, so the app then tracks the
  `.lob` (title, recents, autosave, history). The `.dsgn`'s autosave sidecar
  is cleared too: its content is now saved.
- The GUI says where it went in a notice; `save_project` returns `path` (where
  it was written) and, after a migration, `migrated_from`.

**The rest of the surface** follows: the Save As filter and default name
(`project.lob`), the open dialog's comment, the History panel's message,
`save_project`/`open_project` describe rows, the MCP tool descriptions, the
python client, every generator's default output name, tools (`fuzz.py`,
`reader-fuzz.py`, `bench_workloads.py`, demo scripts). Committed benchmark
results are not touched.

**Packaging.** The desktop file's `Exec` becomes `lobole %f`, and the command
line takes a lone path as `--file` does (a `--file %f` would leave a bare
`--file` on a plain launch, which is refused). No shared-mime-info XML ships:
it could only match a `.lob` FILE, which the app does not write.

Not a format change: no field stores the extension, so no version bump.

## Files

- `src/core/io/Serialization.{h,cpp}` — the folder-name helper, path
  classification, `ProjectSaveTarget`.
- `src/ui/ProjectActions.{h,cpp}` — `ProjectWriteTarget`, the migration in
  `SaveProject`, `MarkProjectSaved`, `IsProjectPath`/`NormalizeProjectPick`.
- `src/ui/HistoryPanel.cpp`, `src/ui/FileDialogs.{h,cpp}`,
  `src/remote/RemoteProject.cpp`, `src/app/main.cpp` (positional path).
- `packaging/org.lobole.Lobole.desktop`, `docs/packaging/README.md`.
- `python/lobole/mcp.py`, `generators/**`, `tools/**`.
- Tests and GUI tests that write `.dsgn` write `.lob`.
- Docs: `docs/versioning/{README,format,history}.md` and every other page
  that names `.dsgn` as the extension; `docs/versioning/README.md` says once
  that `.dsgn` is still read.

## Verification

- Unit (`test_serialization.cpp`, `test_open_project.cpp`): a `.lob` folder
  and a `.json` file round-trip; a `.dsgn` opens; saving the opened `.dsgn`
  writes `<name>.lob` carrying the git log, leaves the `.dsgn` byte-for-byte
  as it was, and tracks the `.lob`; a save to another `.dsgn` is refused and
  writes nothing; an existing `.lob` blocks the migration.
- `tests/startup_file_smoke.py`: `--file` and a positional path on a `.lob`,
  a `.dsgn` and a `.json`; the `save_project` refusal names `.lob`; a
  `save_project` back to an opened `.dsgn` replies with `path` and
  `migrated_from`.
- At least one test keeps opening a `.dsgn`.
- `tools/test-fast.sh`, `tools/ctest -LE gui -j$(nproc)`,
  `./tools/gui-tests-xvfb.sh`, `tools/ctest -L pygui -j1`, `[docs]`.
