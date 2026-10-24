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
| `src/core/io/SerializationInternal.h` | `kFormat`, `kVersion` (`kVersionLanded` + `kVersionPending`), shared JSON helpers |
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
in [format.md](format.md#version-history). On a branch the number is the
placeholder `vNEXT`, and `tools/merge-branch.sh` numbers it as the branch
lands ([how](format.md#a-bump-takes-its-number-when-it-lands)). All of it is
gated by `tests/test_golden.cpp` and `format_version_gate`.

### What a load refuses

A reader given a damaged file does one of two things: refuses it, naming the
file and what is wrong, or loads it correctly. It never loads a document
that is silently missing content, because the next save would write over
the original without it. Every reader opens a `readctx::FileContext`
(`src/core/io/ReadContext.h`) around each file it reads, so any refusal
from inside it arrives as `features/7-fillet.json: features[...]: ...` (a
project container's documents add their directory: `parts/stand/…`), never
as nlohmann's bare `type_error.302`.

The loader refuses, naming it:

- a file that does not parse, holds git conflict markers (`<<<<<<<`), or
  writes one key twice in an object (`readctx::ParseStrict`; nlohmann keeps
  the last and drops the first), or nests a value deeper than 256 levels
  (`kMaxJsonDepth`: nlohmann copies and prints a value recursively, and ten
  thousand nested brackets ran a copy off the stack);
- a part listing more than 50 000 features (`kMaxPartFeatures`, beside
  `kMaxJsonDepth` in `src/core/io/ReadContext.h`): "a part holds at most
  50 000 features; this one lists 104 857". Both part loaders check the
  `features` list (the single file's, the directory's index) before one
  feature is built, so the refusal costs the parse and nothing more: a v1
  box grown to 104 857 copies of its feature had rebuilt past 8 GB. It is
  a sanity bound, not a default: the largest real part here holds a few
  hundred, and one near the limit would take far too long to rebuild to
  be used. An assembly's occurrences are not limited. So that the app
  never writes a part it cannot open, `add_feature` (a `batch` too, and
  `derive_occurrence`) refuses the feature that would take a part past it,
  naming the limit, and the tool window shows the same refusal and keeps
  Accept disabled. If real work ever reaches it, it is raised in that one
  place;
- a key no reader of that object knows, naming it and every key that is
  accepted (`readctx::RefuseUnknownKeys`, in every `…FromJson`, in the
  envelopes and in `project.json`). A newer build's keys come with a newer
  version, which is refused too. A key a reader accepts and does not use (a
  written type tag, `next_entity_id`, a CAM op's `tool_number`) must still be
  what it says (`readctx::RequireTag` / `RequireCount`);
- a version newer than `kVersion`, one that is not a whole number, one older
  than its kind of document (assemblies arrived at 23, drawings at 30, jobs
  at 73), and a part whose features include a type that arrived after the
  version it claims: the migrations for that older version would rewrite
  content that never needed them;
- two files claiming one id (`15-box.json` and `15-hole.json`), an index
  listing an id twice, an index listing an id whose file is not there, an
  item file whose `id` is not the one its name starts with or that has none
  — per collection: features, studies, study results, configurations,
  occurrences, mates, relations, assertions, motion studies, drawing views
  and dimensions — and in a single file, two items of one list sharing an
  id, an id that is not a whole number above 0, or an item other than a v1
  feature with no id;
- a sketch with two entities sharing an id; a parameter defined twice; a
  Cope's or a Derived feature's id list naming one id twice (a damaged file
  listed one a million times), and a CAM operation listing one target
  twice;
- an asset an image or a Derived snapshot names and `assets/` (or the
  single file's `assets` map) does not hold, naming the feature file that
  names it too (the damage may be in that name);
- a document of another kind; in `project.json`, a document entry with no
  `type` or one this build does not know, a `dir` that is not there, an id
  listed twice, more section cuts than the renderer carries
  (`kMaxClipPlanes`, 4, which is all `AddSection` allows);
- a name, a path or a symlink that would reach outside the project
  ([format.md](format.md#paths-in-a-project));
- a value of the right type that a writer would not write back: a
  fixed-size array of another size (`[x, y, z]` with a fourth number, an
  explode step's offset of four), half an element reference, an element of
  no feature where the writer writes only a set one (a sketch's `face_ref`
  with feature 0), a constraint point ref out of its range, a leader that
  points at nothing, an `fcf` frame on a note that is not one, a flat mill
  with a `tip_angle`, tabs without a count, an `active_configuration` the
  document does not have, a sketch plane whose axis lies along its normal,
  a `rollback` past the last feature, a Hole's `thread_depth` or
  `drill_point_deg` below 0 (written only above it), an `angle_deg` or
  `value_deg` past a full turn (written as radians, which the next read
  would refuse), a print row's `to_mm` at or past the and-up sentinel, a
  template instance's value naming no parameter of its definition;
- an id that is not one: not a whole number (2^64 parses as a float and
  read as 0, "none": an `active_configuration`, a CAM target's `part`, a
  drawing view's `base_view`), 0 where the writer writes only a real one (a
  relation ride's `mate` of -0), a sketch entity id past the 32 bits it is
  stored in (2^53 + 1 narrowed to entity 1), an instance or generation id in
  a Derived copy;
- a key the writer writes only beside another, without it: a relation's
  `ride` without its `teeth`, one of `teeth_a` / `teeth_b`, an occurrence's
  `attach_offset`, `attach_rotation` or `attach_seat` without
  `attached_to`, a CAM stock's `size` or `place` without mode `"fixed"`, a
  `finish_stock` without its `finish_tool`, a clearance assertion's
  `measured_mm`, `checked_key` or `note` without its `verdict`, an assembly
  sequence without steps, a `name` beside a print envelope's `profile`
  (which would write over it), and a key of another kind or analysis (a
  spline Curve3D's `axis`, a static study's `num_modes` or topology keys);
- a pre-v4 sketch listing one constraint twice: the migration drops a
  constraint on a rigid rect, and a list grown to 155 344 copies of one
  lost every copy.

A save refuses an item with id 0 (see [format.md](format.md#stability-guarantees)).

### A lossy load

A load that keeps going past damage reports it: one `readctx::Drop` per
file it could not keep — `{path, pointer, reason, derived}` — collected by
`readctx::CollectDrops` into the `LoadReport` that travels with the read
(`ui::LoadedProject::drops`, then `AppState::openDrops`). What loads around
damage, rather than refusing:

- a file in a collection directory that the index does not list, or whose
  name starts with no id: not loaded, and a save would delete it (a save
  prunes every collection directory, an emptied one too, so the app does
  not leave one behind itself);
- an asset nothing names (the single file's `assets` map);
- derived data, marked `derived`: a study or motion result that does not
  read (or whose `.bin` is not the size its json says), a drawing view's
  saved projection that does not read, and baked geometry that cannot be
  used for damage rather than age (unreadable, a link out of the project,
  or not matching its `content_hash`). It is rebuilt.

A document that loaded with a drop that is not derived is not saved back
over the path it came from by default, because that save would lose the
content for good (`ui::SaveProject`, `SaveWouldLoseDrops`):

- GUI: the menu bar shows "Opened incomplete" (or "Results dropped" when
  every drop is derived) with the list. Save raises "Save over an
  incomplete open?" listing each drop, with three ways out: **Save a
  copy…** (the default: the save dialog, leaving the original as it is),
  **Overwrite anyway** (saves, and logs the drops once more) and
  **Cancel**.
- RPC: `open_project` returns `dropped`; `save_project` to the same path is
  refused, naming each drop, unless the call passes `overwrite_lossy: true`,
  when it saves and its reply's `warning` repeats what was lost.
- A save to any other path (Save As) is never blocked. A save over the
  source clears the drops.

## Baked geometry

A `.dsgn` part is a recipe: a feature list re-executed on open. To make
opening a load rather than a rebuild, a save also writes the built bodies
beside it (`src/core/io/GeometryCache.{h,cpp}`):

```
<part>.dsgn/geometry/
  index.json    geometry_format, chain_hash, occ_version, and per body its
                name, material, producing feature, element map, and what
                the features stamped on it (bends, thread regions, ring
                grips, thread and hole marks, a Derived copy's source body)
  0.brep …      one binary B-Rep per body (BinTools)
```

The element map travels because face and edge names are transient in memory,
and a drawing dimension or a mate referencing a face must still resolve after
a load that executed nothing.

**The blob is only an accelerator.** On load the recipe is read first and
the blob is offered to the document (`Document::AdoptBakedGeometry`). The one
regeneration that follows compares the chain hash it computes
(`Document::TipChainHash`, the same hash the regeneration cache keys
on, over every feature's JSON plus the active configuration's suppression)
with the blob's:

- match: the bodies are installed and nothing executes;
- anything else (absent, truncated, a different `geometry_format`, a hash
  from another recipe, a feature file edited by hand or by a merge): the
  blob is ignored and the document rebuilds.

The index also carries a `content_hash`: FNV-1a over the index itself
(without that key) and every `.brep` it names. The chain hash says which
recipe the blob was built from; the content hash says the blob is still what
was written. A blob whose index or bodies were edited or damaged while the
chain hash survived (a body list grown, an element map changed) would
otherwise be adopted whole; it is ignored, the part rebuilds, and the load
reports a derived drop ([a lossy load](#a-lossy-load)). A blob without one
(an older build's) is read on the chain hash alone, and an older build
ignores the key, so it moved no `geometry_format`. A blob that cannot be used because of damage — not
because it is old or from another recipe — is reported the same way.

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
  bends, its threads (`ThreadRegion`, what a screw may engage, and
  `ThreadMark`, what a thread mill reads; geometry format 3), its hole marks
  (4), its ring grips (5), and a Derived copy's source body (`derived_from`,
  6: how a reopened copy finds the size bands its snapshot carries and its
  source body's print settings). A baked open re-derives none of them, and
  without the threads every screw in a reopened tapped hole read as an exact
  clash.
- **A different OCC version loads the blob anyway**, with a log line.
  Rebuilding could reorder faces and move the element names that dimensions
  and mates reference; the geometry the author saw is the geometry loaded.
- **Only the tip is stored.** The regeneration cache is seeded with
  hash-only placeholder entries (`RegenCacheEntry::bodiesValid == false`)
  plus the tip, none holding a clone of its feature, so a later regeneration
  with no edit is free, but the first edit rebuilds from the start, and so
  does the first return to the opened state (nothing evaluated it in this
  session to return to).

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
| never saved | `unsaved-<instance>.autosave.json` / `unsaved-<instance>.autosave.d/` in the user state dir |

The user state dir is `RuntimePaths::UserStateDir()`: `$XDG_STATE_HOME/lobole`
or `~/.local/state/lobole`, and `%LOCALAPPDATA%\Lobole\state` on Windows.

Every sidecar file is written temp + rename, so each is complete or absent:
a tool whose job is surviving a crash must never offer a half-written
recovery. That is why a multi-document project gets a directory of
per-document files rather than one sidecar written like a project tree.
Unchanged documents are not rewritten. `ui::AutosaveProject` owns the loop
over documents, because only the UI layer knows what documents a project has;
assemblies, drawings and jobs autosave as well as parts.

### Every instance has its own unsaved slot

Every instance on a machine shares the state dir — the user's app, and each
agent instance `headless-preview.sh` starts — so the never-saved slot is
named per instance: `<instance>` is the session's stem
(`20261024-031500-4242`, the name of its crash-report marker,
`crash::BeginSession` → `Autosaver::SetInstanceTag`), or `pid<pid>` in a
process without a session. It was one shared `unsaved.autosave.json`: one
instance's autosave wrote over another's recovery copy, and any instance's
save, or clean exit with nothing open, deleted it. Naming it by the session
is what keeps crash recovery working: the session marker lists the slot by
its full name, so the next start copies exactly the dead session's slot
into its report.

A slot outlives its session after a dirty exit, and nothing offers it
again. `crash::CollectDeadSessions`, after copying the dead sessions'
slots into their reports, prunes the slots no live session owns beyond the
newest ten (`kKeepUnsavedSlots`; the shared slot older builds left counts as
one of them) — never a live session's, never its own.

A clean save clears every sidecar of the project it saved, and this
instance's unsaved slot — never another instance's. Recovery:

- GUI: on open, a "Recover autosave?" modal names how many documents have
  newer work; accepting lays every recovered document over the project
  (`ui::RecoverProject`). All or nothing — half from one session and half
  from another is a state nobody can reason about: every sidecar is read
  before any is laid over the project, and one that does not load refuses
  the recovery with the loader's own reason, naming the sidecar
  (`the autosave of parts/Stand does not load: …`). The project stays dirty.
  A one-document project's single sidecar is opened as the document it is;
  one that does not load leaves the project as it was and says why.
- After a crash: the next start's "Loböle closed unexpectedly" notice offers
  the same recovery for the project the crashed session had open, or the
  report's copy of the unsaved slot for one never saved
  ([crash reports](../architecture.md#crashes-and-stalls)).
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

## A shared project's history

A project folder that arrives with its own `.git` (in a zip, say) brings
that repository's config, and config can make git run commands:
`core.fsmonitor` on any diff or status, filter and diff drivers on add and
diff, credential helpers and `uploadpack` on fetch, includes that pull in
more. The app runs git on open (the conflicts scan) and on every save, so
`GitVault` never trusts it:

- every git call passes `-c core.fsmonitor=false -c core.hooksPath=/dev/null`,
  and `-c protocol.ext.allow=never`: an `ext::` URL is a command, and a
  person's own config could otherwise allow it for a shared repository's
  remote;
- a remote's name, a URL, a branch or a revision follows `--` or
  `--end-of-options`, so one that starts with `-`
  (`--upload-pack=<command>`) is a name and never an option;
- before git runs in a project, `UntrustedReason` reads `.git/config` as a
  plain file (`git config --file … --list`, which runs nothing) and accepts
  only keys that run nothing (`HarmlessConfigKey`: core layout, user,
  remotes' urls and refspecs, branch upstreams, a few more). Anything else,
  or a `.git` that is a file, and git does not run there at all: `IsRepo` is
  false, `Init` refuses to reinitialize, and the History panel says which
  key and how to remove it;
- git runs only in the project folder's own repository, never in a parent
  folder's.

A repository fetched with `git clone` carries no config or hooks, so a
cloned project's history is used as it is.

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
| `open_project {path}` | open; returns `autosave_available`, `autosave_path`, `rebuilt_parts`, `rebuild_reason`, `dropped` ([a lossy load](#a-lossy-load)), regen |
| `save_project {path, message?, force?, overwrite_lossy?}` | save; `message` is the git commit message; refuses when the disk moved unless `force`, and over a lossy open unless `overwrite_lossy` (then its reply carries `warning`) |
| `autosave_now`, `autosave_status` | force an autosave; list recoverable documents |
| `project_changed_on_disk` | the stamp check |

History, merge and remote RPCs are listed in [history.md](history.md#rpcs).

## Reader fuzzing

No hand-picked case damages a file the way a disk, a sync client or a merge
does, so a sweep does: `tests/test_reader_fuzz.cpp` hands every reader
damaged versions of a valid input and holds each case to the rules above.

**The damage** (`tests/mutate/Mutate.{h,cpp}`) is seeded and reproducible.
Each case has a stable id, `<corpus>/<file>:<operator>:<n>`, and is a pure
function of the input, the id and the base seed (xorshift64 seeded by a hash
of the id), so one case replays without the others. Five levels:

- bytes: truncation (every length of a file under 1 KB; else the first and
  last 64 and 1024 spread between), bytes flipped, zeroed and set to 0xff,
  runs dropped, repeated and swapped, garbage appended, the file doubled,
  reversed, emptied;
- text: lines dropped, repeated, swapped and replaced, a word replaced by
  another of the file's, each number in turn and then all of them replaced
  by a liar (the values the hostile sweep sends,
  [rpc.md](../scripting/rpc.md#the-hostile-sweep));
- JSON: a key deleted or written twice, keys reordered (which must load
  identically), a value's type swapped, a number lied about, an enum string
  with one character changed, `version` set to 0, 1, `kVersion`,
  `kVersion + 1`, -1, a string or removed, a value wrapped 10 000 arrays
  deep, an array grown to 10⁶ copies of its first element (at most 8 MB of
  text);
- model: an id reference pointed at no item, at its own or at a later one;
  two items given one id; id 0; items reordered;
- directory: a file deleted, emptied, duplicated under another id or
  unlisted; a document's index swapped for another kind's; a `dir`, asset
  or blob name that leaves the project; a symlink out of it; a result's
  `offset`, `count` or `bin_bytes` lying.

**The corpus** is every golden in its single-file form and, saved by this
build, in its directory form (with a saved study result on
`v153-saved-study-results`, a motion result on `v87-motion-study`, and the
committed `v162-stored-projection` drawing), one project container of every
document kind with variables, a folder, a section and a requirement; and,
for the readers outside documents, fixtures written by the test: a settings
file with every key, a keymap and a window; a machine and a print profile
store; a dead session's marker, crash file and log beside an earlier
report; the instance registry; a project's autosave sidecars; a git vault
with two commits (a damaged version committed, logged, extracted, loaded,
and scanned by `ProjectMerge`); a PNG and a JPEG for `stbi_info_from_memory`
and the underlay's decode. A tree's own file damage runs on every file that
is no collection's item and on one item of each kind per directory; the
single-file families damage every item's content.

A single-file input must be laid out as the writer saves it (`dump(2)`),
so one dropped line breaks the JSON or takes a key. A golden written by
hand with a whole feature, occurrence or view on one line loses it to
`drop_line` and stays valid: a smaller document, which no reader can tell
from damage. The sweep refuses such an input before its cases, naming the
line and the list.

**The rules** for every case:

- crash: the process dies, or ASan / UBSan reports. Before each case its id
  goes to stderr and to `build/reader-fuzz-last-case.<family>` (one file per
  family: ctest runs them side by side), so a crash or a hang names it;
- refused without a name: the message must hold the damaged file's path
  relative to what was read (or, for a file a directory operator removed or
  added, its directory and id) and something beyond it. Damage to derived
  files only must not refuse the document at all;
- silently missing content: a loaded case is saved, and every value of the
  input that is not in the save — past what the unmutated input's own round
  trip loses, counted by shape so a re-keyed item still matches — must be
  covered by a drop. A value the writer leaves out as its default is not
  lost: changed, it changes the save (or is refused), which is how the
  harness tells it from a value the reader ignores. The saved copy must load
  and save again to the same bytes (geometry aside: a rebuilt body and one
  read back write different B-Rep bytes). A sketch's projected entities
  (an external reference's `gen_ids`) are derived, like bodies: every
  regeneration projects them again, so damage to the sketch the face came
  from changes them as an edit would, and they are not counted. Item
  counts (features,
  occurrences, views, setups, …) must be the original's unless a lossy drop
  was reported; bodies only where the damage cannot change the model;
- budget: 2 s of CPU time a case (20 s under `ENABLE_SANITIZERS`), or four
  times the unmutated input's read when that is longer (a part that rebuilds
  its blob takes its rebuild's time), plus half that for every megabyte the
  damage wrote (a grown array is 8 MB of honest parsing; a quadratic reader
  takes minutes on it). CPU time, not wall: a read on a machine at load 60
  waits for a core (2.42 s of wall time against 2 s, three times in a
  night), and a reader that hangs spinning or goes quadratic spends CPU
  either way. It is the PROCESS's CPU, because a regeneration can run
  `parallel::For` workers, plus the children it waited for (the vault's git
  processes). A wall-time backstop of fifteen times the budget catches a
  reader that waits instead -- on I/O or a lock -- and the failure names
  which clock tripped, with both times. A reader that never returns is
  stopped by ctest's `TIMEOUT` (1200 s), and the last-case file names it.

The non-document families have rules of their own: settings never throw
and a file that does not parse leaves a note naming it; a profile store
loads every file or names it as unreadable; collecting crash reports never
throws and copies nothing from outside the state dir; the registry never
reports an instance with pid or port 0; a recovery refuses naming the
sidecar; an image decodes to the size its header states, or not at all.

**Registration.** One `TEST_CASE` per family, so ctest runs them in
parallel. `[reader_fuzz]` is the fixed sample: every operator at least once
per family, about 1/50 of the rest, over a sixth of the corpus (each input
costs a load and a save before its first case) plus the inputs with side
files; it is in `tools/test-fast.sh` and is what `tools/ci-local.sh --asan`
runs. `[reader_fuzz][slow][asan_slow]` is every case of the whole corpus,
in the headless suite (under ASan one family alone ran past ctest's 20
minutes); the part files and the part directories run as four shards each
(`… 1/4` to `4/4`, split by a hash of the input's name). Two exceptions keep
it to minutes: an input whose own read takes a quarter second or more (a
modeled thread, a worm, a text sketch — most of its damaged cases rebuild
it) runs every operator once and an eighth of the rest, and the git vault
runs a handful of operators, each case costing six git processes. Each
prints its count and time.
`Reader fuzz: the harness catches what it claims to` plants a reader that
drops an unknown key, one that refuses without naming the file, one that
spins past its CPU budget and one that sleeps past a (shortened) wall
backstop, and each must fail its rule; a part with its feature on one line
must be refused as an input, and the golden it was taken from must not. `reader_fuzz_budget_under_load`
pins a reader spending half its budget in CPU to one core beside four
contending `tools/with-cpu-load.sh` burners: its read takes about five times
that in wall time, past the budget, and must pass.

**By hand.** `tools/reader-fuzz.py run --seed S [--family F] [--only TEXT]`
reruns the full sweep with another base seed (ctest's is fixed: a seed that
moved would make the suite's result depend on the day) and prints the
failures; `LOBOLE_READER_FUZZ_OPS=a,b` narrows a run to operators. Run it
under `systemd-run --user --scope -p MemoryMax=8G`: nothing but ctest's
`TIMEOUT` stops a case, and before `kMaxPartFeatures` one reached 23 GB.
Every family takes about 25 minutes a seed. Seeds 1 to 3 over every family
run clean; what they found is the refusals above and the cases in
`tests/reader_cases/`, by kind: a number past an integer's range (2^64,
2^53 + 1, -0) read into an id; a key the writer writes only beside
another; a list longer than the app ever writes; a feature built on a
sketch that failed, which spun a boolean for minutes.
`tools/reader-fuzz.py case <id>` writes one case — the damaged file or tree,
as bytes, not the id — to `tests/reader_cases/<name>/input/` with an
`expect.json` (`refused` and the substrings its message holds, `loads` with
its counts and drops, or `passes` for a family outside documents), which
`tests/test_reader_cases.cpp` loads; every bug the sweep found is one of
these, and each failed before its fix. `LOBOLE_READER_CORPUS=<dir>` runs the
hidden `[.reader_corpus]` case over generator output under `scratch/`
(untracked, so it never decides whether the suite passes).

## Tests

| where | covers |
|---|---|
| `tests/test_golden.cpp` `[golden]` | the corpus loads, regenerates and round-trips; newest golden and this history reach `kVersion` |
| `format_version_gate` (`tools/format-version.py check`) | `kVersion` is `kVersionLanded` + `kVersionPending`; the pending rows, golden names and `vNEXT` placeholders agree with it |
| `tests/test_serialization.cpp` `[io]` | round trips, directory form; duplicate id files, duplicate index ids and conflict markers refused; unknown feature, entity and mate types refused by name |
| `tests/test_baked_geometry.cpp` `[baked]` | a baked open executes nothing; a recipe edited behind the app's back and a broken blob rebuild; a failed model is not baked; datums, warnings and bends survive a baked open; `commit_geometry` off |
| `tests/test_autosaver.cpp` `[autosave]` | sidecar paths, fires only when dirty + changed + due, a replaced document at the same version still autosaves; `Clear("")` takes this instance's unsaved slot and no other's; orphaned slots pruned beyond the count, never a live one |
| `tests/autosave_instances_smoke.py` | two instances on one state dir: each autosaves its own unsaved slot, and one's save leaves the other's alone |
| `tests/test_save_stamp.cpp` `[savestamp]` | each kind of saved edit that regenerates nothing (tolerance, units, tube, print process, configuration, study, asset; assembly units and undo; drawing undo; a job's GUI and RPC edits and undo) reads dirty and clean after a save; a study run is not an edit; autosave writes them |
| `tests/test_conventions.cpp` | every `Document` mutator regenerates or marks the edit; no new mutable accessor |
| `tests/unsaved_edits_smoke.py` | the same edits over the RPC read `ping.dirty` until saved; reading the model does not |
| `tests/test_projectstamp.cpp` `[stamp]` | stability, external edits, `.git/` and `geometry/` ignored at every depth, invalid never matching |
| `tests/test_command_modify.cpp` | a modified feature keeps its id; no id-0 item is written |
| `tests/headless_smoke.py` | autosave across a SIGKILL, the disk guard end to end with `force` and Save As |
| `tests/test_reader_fuzz.cpp` `[reader_fuzz]` | the mutation library's own test, the harness's planted readers, and the sweep ([reader fuzzing](#reader-fuzzing)) |
| `tests/test_reader_cases.cpp` | every regression in `tests/reader_cases/` reads as its `expect.json` says; a part past `kMaxPartFeatures` is refused before it is built, in seconds and under 100 MB, in either form |
| `tests/feature_limit_smoke.py` | `add_feature` (and a batch) past `kMaxPartFeatures` is refused naming the limit, changing nothing; a part at the limit saves and opens |
| GUI `document/accept_refused_past_the_feature_limit` | the tool window's Accept is disabled past the limit, and the keyboard's accept does nothing |
| `tests/lossy_open_smoke.py` | `open_project` reports an unlisted feature file in `dropped`; `save_project` over it is refused until `overwrite_lossy`, and a save elsewhere is not |
| GUI `disk/a_lossy_open_asks_before_saving_over` | Save raises the choice; Cancel keeps the file, Overwrite anyway saves over it (Save a copy… is not clicked: it opens the system dialog) |
| GUI `autosave/recovers_every_document` | a multi-document project autosaves each dirty document and recovers all of them |
| GUI `disk/*` | the moved-project modal and the History banner (the menu-bar notice itself is not asserted: a disabled main-menu `MenuItem` does not resolve through the test engine; the flag behind it is) |

## Not done

- A real file watcher (inotify and friends): focus plus a slow timer plus the
  save-time guard covers the workflow.
- Baking a regeneration history (per-feature snapshots), so the first edit
  after opening is fast too.
- Git LFS for the baked blobs; `commit_geometry: false` is the size opt-out.
