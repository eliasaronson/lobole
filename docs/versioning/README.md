# Versioning: projects on disk

How a project is stored, how the app keeps memory and disk honest with each
other, and how work survives a crash. The git-backed history, merging and
sharing are in [history.md](history.md); the document format's contract and
version history are in [format.md](format.md).

## A project on disk

A project has one of three shapes, chosen by `ui::SaveProject`
(`src/ui/ProjectActions.cpp`):

| shape | when | layout |
|---|---|---|
| single file `.json` | one part saved to a `.json` path (`save_project`, scripts); the GUI's Save As always writes `.dsgn` | one JSON document, assets base64 inside it |
| flat `.dsgn` directory | one part, no part-local variables, no tab folders, no section cuts | `index.json` + `features/` + … (see [format.md](format.md#the-directory-form)) |
| project container `.dsgn` | anything else: several documents, or an assembly, drawing or job | `project.json` + `parts/<name>/`, `assemblies/<name>/`, `drawings/<name>/`, `jobs/<name>/`, each a document directory |

The GUI's Save As turns any other name into `<name>.dsgn`
(`DocumentIO::ProjectSaveTarget`: `.json` is replaced, anything else
appended). `.json` is the
single-document interchange form only: saving a multi-document project to a
`.json` path is refused, naming the `.dsgn` path to use instead, because the
container would otherwise be a directory named `.json` without the git
history, which keys on `.dsgn`. Open dispatches on the path: a `.dsgn` or
any directory opens as a project (`project.json` present means the container
form), a file as a single document. Picking an `index.json` inside a `.dsgn`
opens the project.

In the flat form the one part's file carries the project-scope variables; in
the container they live in `project.json` and each part's `index.json`
`parameters` are that part's local scope.

### Serialization modules

| file | holds |
|---|---|
| `src/core/io/SerializationInternal.h` | `kFormat`, `kVersion` and its per-version comments, shared JSON helpers |
| `src/core/io/Serialization.cpp` | envelopes, `Save`/`Load`, `SaveDir`/`LoadDir`, assemblies, drawings, jobs, a lone part's project tables, requirements and templates, the study results' and drawing projections' files, load refusals |
| `src/core/io/SerializeFeatures.cpp` | `FeatureToJson` / `FeatureFromJson`, sketch entities and constraints |
| `src/core/io/SerializeAssembly.cpp` | occurrences, mates, relations, exploded views, motion studies and their saved results, the clearance assertions an old file carries |
| `src/core/io/SerializeStudies.cpp` | FEM and topology studies and their saved results (the `.result.bin` arrays) |
| `src/core/io/SerializeDrawingProjections.cpp` | a drawing view's saved projection (the `.projection.bin` arrays) |
| `src/core/io/SerializeConfigurations.cpp` | part and assembly configurations |
| `src/core/io/SerializeCam.cpp` | CAM setups, tools, operations |
| `src/core/io/AtomicWrite.cpp` | `WriteTextAtomic`: temp + rename, skips identical content |
| `src/ui/ProjectActions.cpp` | the project container, open/save/recover, staged-import adoption |

A format change bumps `kVersion`, adds a golden at the new version and a row
in [format.md](format.md#version-history). All three are gated by
`tests/test_golden.cpp`.

### What a load refuses

The loader says what is wrong rather than guessing:

- a file holding git conflict markers (`<<<<<<<`), named;
- two files claiming one id (`15-box.json` and `15-hole.json`), and an index
  listing an id twice — per collection: features, studies, study results,
  configurations, occurrences, mates, relations, assertions, motion studies,
  drawing views and dimensions;
- a sketch with two entities sharing an id;
- a document of another kind, or a version newer than `kVersion`.

A save refuses an item with id 0 (see [format.md](format.md#stability-guarantees)).

## Baked geometry

A `.dsgn` part is a recipe: a feature list re-executed on open. To make
opening a load rather than a rebuild, a save also writes the built bodies
beside it (`src/core/io/GeometryCache.{h,cpp}`):

```
<part>.dsgn/geometry/
  index.json    geometry_format, chain_hash, occ_version, and per body its
                name, material, producing feature and element map
  0.brep …      one binary B-Rep per body (BinTools)
```

The element map travels because face and edge names are transient in memory,
and a drawing dimension or a mate referencing a face must still resolve after
a load that executed nothing.

**The blob is only an accelerator.** On load the recipe is read first and
the blob is offered to the document (`Document::AdoptBakedGeometry`). The one
regeneration that follows compares the chain hash it computes
(`Document::TipChainHash`, the same hash the regeneration prefix cache keys
on, over every feature's JSON plus the active configuration's suppression)
with the blob's:

- match: the bodies are installed and nothing executes;
- anything else (absent, truncated, a different `geometry_format`, a hash
  from another recipe, a feature file edited by hand or by a merge): the
  blob is ignored and the document rebuilds.

A blob that no longer matches the recipe makes an open slower, never wrong.
Consequences:

- **A model with a failed feature is not baked**, and neither is one with no
  bodies. A blob is a claim that this is what the recipe produces.
- **An unchanged save rewrites nothing**: when the blob already carries the
  current hash AND the bodies in memory were installed from that very blob
  with nothing executed since (`Document::BodiesAreBlob`), it is left alone,
  so repeated saves add nothing to git. The hash names the recipe, not the
  code that ran it: a part this build regenerated, or one opened from another
  project and saved here, is written even under a matching hash (a body whose
  bytes did not change keeps its file), or a kernel fix would never reach a
  project saved before it.
- **Datums still execute on a baked open.** A datum plane, axis, frame or
  curve (`Feature::IsDatum`) produces state rather than a body, and the blob
  carries only bodies; skipping them would leave a mate anchored to a Frame
  at the identity. They run against the installed bodies, in list order.
  Feature warnings recorded at bake time are restored.
- **What features stamp on a body travels with it**: its element map, its
  bends, and its threads (`ThreadRegion`, what a screw may engage, and
  `ThreadMark`, what a thread mill reads; geometry format 3). A baked open
  re-derives none of them, and without the threads every screw in a reopened
  tapped hole read as an exact clash.
- **A different OCC version loads the blob anyway**, with a log line.
  Rebuilding could reorder faces and move the element names that dimensions
  and mates reference; the geometry the author saw is the geometry loaded.
- **Only the tip is stored.** The prefix cache is seeded with hash-only
  placeholder entries (`RegenCacheEntry::bodiesValid == false`) plus the tip,
  so a later regeneration with no edit is free, but the first edit rebuilds
  from the start.

`Document::CopyWithGeometry` uses the same mechanism for an in-memory copy
(a motion study's snapshot of an assembly): the copy is offered the source's
bodies and keeps them only if the hash agrees, otherwise it rebuilds. It
returns a pointer on purpose: `make_unique<Document>(CopyWithGeometry(d))`
on a by-value return would bind the copy constructor and regenerate anyway.

When a project's parts cannot use their blobs, the open rebuilds them side by
side, the menu bar shows "Geometry rebuilt" until a save re-bakes, and
`open_project` reports `rebuilt_parts` and `rebuild_reason`
("no saved geometry", "from an older build", "from a newer build", or "the
recipe changed since the geometry was saved"; `DocumentIO::InspectGeometry`
reads the first three without loading the bodies).

**In git.** Geometry is committed by default, so a clone or an old checkout
opens fast and the geometry is pinned against kernel drift. Every save writes
`.gitattributes` marking `geometry/*.brep binary -merge -diff`: a conflict
on derived data is resolved by taking either side, and the hash decides
whether it survives. `commit_geometry: false` in a part's `index.json` turns
it off: `geometry/` is deleted and a `.gitignore` keeps it out. Adding a git
remote sets this on every part (see [history.md](history.md#sharing-through-a-remote)).

## Unsaved changes

Whether the project is dirty is one question — the close prompt, the
window title's `*`, `ping.dirty` and autosave all ask `ui::ProjectDirty` —
and it is answered by each document's **save stamp**, not its version.

`Document::Version()` moves only when the model regenerates, and a part
carries saved state that regenerates nothing: its tolerances, units, tube
rules and print process, its studies, a configuration that is not the
active one, its image assets. Compared by version, an edit to any of those
read clean and was dropped on close without a prompt. So
`Document::SaveStamp()` is the version plus a count of those edits
(`MarkEdited`), and the rule has three backstops:

- **Every mutator of saved state regenerates or calls `MarkEdited`.**
  `test_conventions.cpp` scans `Document.h` and fails on a new `Set*`/
  `Add*`/`Remove*`/... that does neither, or on a new non-const accessor
  into saved state — the way round every mutator. Studies and
  configurations are read-only from outside (`ReplaceStudy`,
  `ReplaceConfiguration` edit them); `StudyForRun` is the one write door,
  for a solve's transient fields.
- **Every command, undo and redo stamps** (`CommandStack`), whatever it
  touched. An undo back to exactly the saved content still reads dirty: the
  stamp only grows, which errs towards one prompt too many.
- **The other document kinds have their own**: `AssemblyDocument::SaveStamp`
  (its units), `cam::JobDocument::
  SaveStamp` (a job's `Version()` is only a cache key; `MarkJobEdited` and
  every `JobUndoGuard` that records a step count the edit, which is how the
  CAM RPCs reach it), and a drawing's `Version()`, which every drawing edit
  bumps. An assembly, drawing or job undo restores a snapshot, so the
  restore moves its version and stamp PAST the current ones — the snapshot's
  own could equal the stamp the last save took.

Each slot remembers the stamp it was saved at (`AppState::lastSavedVersion`
for the active tab, `PartSlot::lastSavedVersion` for the rest), and
`ProjectDirty` is true when any slot's stamp differs or `projectMetaDirty`
is set — which is now only for what belongs to no document: adding,
deleting or renaming a document, folders, catalogs, requirements and their
evidence. `MarkProjectSaved` /
`MarkProjectOpened` re-stamp every slot. Opening a sketch moves the view
rollback and regenerates without being an edit, so it re-stamps a clean
document (`SetViewRollbackKeepingDirt`).

A version number is not an identity: versions restart at 0 whenever a
document is replaced, so anything that tracks "this document at this
version" also keys on `Document::InstanceId()`.

## Autosave and recovery

`Autosaver` (`src/core/io/Autosaver.{h,cpp}`) is pumped from both the GUI
frame loop and the headless loop; `--autosave <seconds>` sets the interval
(default 60, 0 = off). A document is written when the interval has elapsed,
it is dirty, and it changed since its last autosave — all three measured by
the save stamp ([above](#unsaved-changes)), so a tolerance-only edit is
autosaved like any other. A clean exit (the stamp at the last save, every
slot) removes the sidecar; a dirty one keeps it.

Where it goes:

| project | sidecar |
|---|---|
| one document | `<project>.autosave`, a single file |
| several documents | `<project>.autosave.d/<dir>.json`, one file per DIRTY document, mirroring `project.json`'s `dir` (`parts/Stand.json`) |
| never saved | `unsaved.autosave.json` / `unsaved.autosave.d/` in the user state dir |

The user state dir is `RuntimePaths::UserStateDir()`: `$XDG_STATE_HOME/lobole`
or `~/.local/state/lobole`, and `%LOCALAPPDATA%\Loböle\state` on Windows.

Every sidecar file is written temp + rename, so each is complete or absent:
a tool whose job is surviving a crash must never offer a half-written
recovery. That is why a multi-document project gets a directory of
per-document files rather than one sidecar written like a project tree.
Unchanged documents are not rewritten. `ui::AutosaveProject` owns the loop
over documents, because only the UI layer knows what documents a project has;
assemblies, drawings and jobs autosave as well as parts.

A clean save clears every sidecar (and the unsaved slot). Recovery:

- GUI: on open, a "Recover autosave?" modal names how many documents have
  newer work; accepting lays every recovered document over the project
  (`ui::RecoverProject`). All or nothing — half from one session and half
  from another is a state nobody can reason about. The project stays dirty.
- RPC: `open_project` returns `autosave_available` and `autosave_path`;
  `autosave_status` lists the documents with recoverable work;
  `autosave_now` writes every dirty document immediately.

The sidecars sit beside the project, outside its directory, so they are
outside its git repository and outside the disk stamp below. A sidecar is
the single-file form, which carries no study result and no drawing
projection: an autosave never writes one, and a recovered document's
studies run again
([what a saved result keeps](../simulation/README.md#what-a-saved-result-keeps))
and its drawing views project again.

## Saved results stay out of the history

A study or motion result is saved beside its study
([studies as jobs](../simulation/README.md#studies-as-jobs)), and a drawing
view's projection beside the view
([saved projections](../drawings/README.md#saved-projections)), and the
project's repository keeps neither: `GitVault::CommitAll` (and
`CommitMerge`) writes `*.result.json`, `*.result.bin`, `*.projection.json`
and `*.projection.bin` into the repository's own exclude file
(`.git/info/exclude`, found with `git rev-parse --git-path`), each pattern
added when its line is missing (so a repository an earlier build set up
gains the projections' lines), and takes any such file an earlier build
committed out of the index (`git rm --cached`), leaving it on disk; `Init`
writes the rule from the first commit.

None, rather than the newest: a result is reproducible from its inputs and
named by their hash, so a version needs none to be what it was, and git
keeps every blob it was ever given -- "only the newest" in a history is a
history rewrite on every run. A few re-runs of the yoke's 52 MB static
result held more than the rest of the project together. The newest result
of each study is the one in the working tree, which git leaves alone:
restoring a version keeps it, and the reopen reads it current when the
restored inputs hash the same and stale when they do not; a clone, or a
teammate's pull, carries no results, and their studies run again there.
Because a result is untracked, a new one is not a change to commit and does
not make the project dirty (`GitVault::Dirty`). History written before v154
keeps the results it holds; nothing rewrites it.

## Changes made outside the app

The app holds documents in memory and the project is a directory, so
something else can write it: a `git` command in the app's own terminal panel,
another editor, a second instance. `DocumentIO::StampProject`
(`src/core/io/ProjectStamp.{h,cpp}`) records `{fileCount, maxMtime,
totalSize}` over the project directory — rsync's quick check, not a content
hash. Git writes files with the current mtime on checkout, merge, restore and
pull, so even putting an older tree on disk moves it. It skips two
directories: `.git/` (not the document, and it churns on its own) and
`geometry/` (derived data; the chain hash decides at load). The stamp covers
the recipe. An invalid stamp never matches anything, including itself.

What it cannot catch: a write in the same mtime tick that preserves the exact
byte count. Coarse mtimes or clock skew on a network filesystem widen that
window; the check degrades to "usually notices", never to "wrong".

The stamp is taken in `ui::OpenProject` itself (both of its success paths:
a single-part `.dsgn` returns early), in `SaveProject` after the write, and
in `MarkProjectSaved` after the git commit so the app's own commit is not
mistaken for an external write.

- **The guard.** Every save goes through `ui::SaveProject` — Ctrl+S, Save As,
  naming a version, `save_project`. When the target is the opened path and
  the disk moved, it refuses. The GUI offers **Reload from disk**, **Save
  anyway** and Cancel; `save_project {force: true}` saves over it.
- **The poll.** `PollDiskState` (`src/ui/HistoryPanel.cpp`) runs once a
  frame from the top of the UI, not inside a panel (a panel behind another
  tab does not run). It checks when the window regains focus and on a slow
  timer, since a headless agent can write while the window keeps focus. With
  git it first compares HEAD, one `git` call instead of a walk.
- **The notice.** A "Changed on disk" label in the main menu bar — text, not
  a button, since both actions discard something — and a banner with Reload
  at the top of the History panel, above its git checks so projects without
  history are warned too. When the change left merge conflicts, the Conflicts
  panel speaks instead.
- `project_changed_on_disk` lets a script ask before it saves.

Merging in-memory edits against a moved disk is not done: "reload or
overwrite" is the choice.

## Imported geometry inside a project

An assembly import extracts one `.brep` per unique product. Before the
project has a folder these land in `<user state dir>/import-staging/`; the
first save into a directory copies them to `<project>/imported/` and
repoints the Import features (`AdoptStagedImports` in `ProjectActions.cpp`),
so a saved project is self-contained. See
[../interchange/README.md](../interchange/README.md#importing-an-assembly).

## RPCs

| RPC | does |
|---|---|
| `new_project` | reset to one empty part, discarding what is open |
| `open_project {path}` | open; returns `autosave_available`, `autosave_path`, `rebuilt_parts`, `rebuild_reason`, regen |
| `save_project {path, message?, force?}` | save; `message` is the git commit message; refuses when the disk moved unless `force` |
| `autosave_now`, `autosave_status` | force an autosave; list recoverable documents |
| `project_changed_on_disk` | the stamp check |

History, merge and remote RPCs are listed in [history.md](history.md#rpcs).

## Tests

| where | covers |
|---|---|
| `tests/test_golden.cpp` `[golden]` | the corpus loads, regenerates and round-trips; newest golden and this history reach `kVersion` |
| `tests/test_serialization.cpp` `[io]` | round trips, directory form; duplicate id files, duplicate index ids and conflict markers refused; unknown feature, entity and mate types refused by name |
| `tests/test_baked_geometry.cpp` `[baked]` | a baked open executes nothing; a recipe edited behind the app's back and a broken blob rebuild; a failed model is not baked; datums, warnings and bends survive a baked open; `commit_geometry` off |
| `tests/test_autosaver.cpp` `[autosave]` | sidecar paths, fires only when dirty + changed + due, a replaced document at the same version still autosaves |
| `tests/test_save_stamp.cpp` `[savestamp]` | each kind of saved edit that regenerates nothing (tolerance, units, tube, print process, configuration, study, asset; assembly units and undo; drawing undo; a job's GUI and RPC edits and undo) reads dirty and clean after a save; a study run is not an edit; autosave writes them |
| `tests/test_conventions.cpp` | every `Document` mutator regenerates or marks the edit; no new mutable accessor |
| `tests/unsaved_edits_smoke.py` | the same edits over the RPC read `ping.dirty` until saved; reading the model does not |
| `tests/test_projectstamp.cpp` `[stamp]` | stability, external edits, `.git/` and `geometry/` ignored at every depth, invalid never matching |
| `tests/test_command_modify.cpp` | a modified feature keeps its id; no id-0 item is written |
| `tests/headless_smoke.py` | autosave across a SIGKILL, the disk guard end to end with `force` and Save As |
| GUI `autosave/recovers_every_document` | a multi-document project autosaves each dirty document and recovers all of them |
| GUI `disk/*` | the moved-project modal and the History banner (the menu-bar notice itself is not asserted: a disabled main-menu `MenuItem` does not resolve through the test engine; the flag behind it is) |

## Not done

- A real file watcher (inotify and friends): focus plus a slow timer plus the
  save-time guard covers the workflow.
- Baking a regeneration history (per-feature snapshots), so the first edit
  after opening is fast too.
- Git LFS for the baked blobs; `commit_geometry: false` is the size opt-out.
