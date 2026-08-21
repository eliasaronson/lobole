# F8 — When the project changes underneath you — design

> **Status: not built.** Planned 2026-08-21, out of [F7](f7-merge-and-conflicts.md)'s
> scoping, which found this gap while looking for something else. Half a day
> for the part that prevents data loss; the rest is polish.

## Why this exists

A Designer project is a directory, and the app holds its documents in memory.
Nothing checks that the two still agree. `ProjectDirty` compares *document
versions* — an in-memory number — and the only filesystem comparison anywhere
is `Autosaver::HasNewerAutosave`, which weighs the sidecar against the project
(`Autosaver.cpp:33`) and says nothing about the project's own contents.

So: something else writes the project directory, the app never notices, and
the next Ctrl+S writes memory over it. The external change is gone, with no
message and no prompt.

This was always true and mostly theoretical. Two things made it reachable:

- **The terminal panel runs inside the app**, bound to this instance and
  started in the open project's directory (`TerminalPanel.cpp:116-130`). A
  `git checkout`, `git pull` or `git restore` there moves the working tree
  under the documents in memory. That is the panel working exactly as
  designed.
- **F7 added in-app verbs that move the working tree** — switch, merge,
  restore. Those are safe *today*, because each one calls `OpenProject`
  afterwards, and that is the discipline to keep. But the app now speaks a
  vocabulary where the tree moving is a normal event rather than an
  impossibility.

And one that was always true and is not theoretical at all: **two instances**.
The settings store already merges on save because a windowed `dev-watch`
instance and headless agents run here at once ([../ui/settings.md](../ui/settings.md)).
Two of them on one *project* clobber each other by exactly this mechanism.

## What a user gets

**A save that refuses rather than overwrites.** Ctrl+S on a project that moved
underneath you stops and says so:

```
  This project changed on disk since you opened it
  ────────────────────────────────────────────────
  Something else wrote to Marsliden dock — a command in the
  terminal, another editor, or a second Designer window.

  Saving now would replace it with what is open here.

  [Reload from disk]   [Save anyway]   [Cancel]
```

**Reload** is `OpenProject` — the same primitive restore, branch-switch and
merge-finish already use. **Save anyway** overwrites, and the dialog says what
that costs *honestly and differently* depending on the project: a `.dsgn`
project under git can add "the version on disk is already in your History, so
this is recoverable"; a single-file project cannot, and must not pretend to.

**And a banner before you get there**, so the refusal is rarely the first
news: a line in the History panel when the project has moved, with the same
Reload. The refusal is the half that prevents loss; the banner is the half
that makes it pleasant.

## The stamp

A `ProjectStamp` is `{fileCount, maxMtime, totalSize}` over a walk of the
project directory.

**Why not a content hash.** It would have to read every `.brep` in
`geometry/` on every save and every poll. mtime + size + count is what rsync
calls a quick check, and it is the right trade here: cheap enough to run at
every save on a 48-part project, and it catches everything that actually
happens.

**Why it catches what it needs to.** Git writes files with the *current* mtime
on checkout, merge, restore and pull — so every git operation moves the stamp,
including ones that put an *older* tree on disk. So does another editor, so
does rsync, so does a second Designer instance.

**Two exclusions, both principled:**

- **`.git/` is not the document.** It churns constantly on its own — the index,
  the reflog, and background `maintenance`/`gc --auto` runs. F7 learned that
  the hard way: git's detached maintenance process creates and deletes
  `.git/objects/maintenance.lock` while you are reading the directory, which
  turned `remove_all` into a `terminate()` in a test destructor.
- **`geometry/` is the build, not the recipe.** F7 already settled that baked
  geometry is derived data with no meaningful conflict — either side does,
  because the chain hash decides at load. A guard that fires on derived data
  would contradict that. **The stamp covers the recipe.**

**What it does not catch**, stated rather than glossed: an edit that lands in
the same filesystem mtime tick *and* preserves the exact byte count. That is
essentially a deliberate construction. Network filesystems with coarse mtime
granularity or clock skew widen that window; the guard degrades to "usually
notices" there rather than to "wrong".

## Where it hooks — one guard, not a scattering

The structure already funnels, which is why this is half a day.

**Every save goes through `ui::SaveProject`** (`AppActions.h:432`) — Ctrl+S
(`FileDialogs.cpp:490`), Save As (`:334`), `NameCurrentVersion`
(`HistoryPanel.cpp:142`), and the `save_project` RPC (`RemoteProject.cpp:88`).
One check there covers the GUI, the RPC and the MCP surface at once.

**Every arrival goes through `MarkProjectOpened` / `MarkProjectSaved`**, which
already call `StampAllSaved` for document versions. `StampDiskState` goes
beside it — and in `MarkProjectSaved` it must run **after** `CommitAll`, so
that our own commit is not mistaken for someone else's write.

Two details that fall out of the existing code:

- **Save As has nothing to compare.** The guard only fires when the target
  path is the one we opened *and* a stamp exists for it.
- **A no-op save must not move the stamp.** `WriteTextAtomic` skips identical
  content on purpose, so an untouched feature keeps its mtime
  (`AtomicWrite.cpp`) — which means re-stamping after a save is both correct
  and nearly free.

**The poll** wants a signal, and there is a clean place for one: `main.cpp:1582`
already asks GLFW whether the window is iconified each frame, and
`GLFW_FOCUSED` sits right beside it. Checking when the window regains focus
matches the actual workflow — you switch to a terminal, do a git thing, switch
back — and costs nothing while you are working. For a git project, one
`git rev-parse HEAD` is a cheap pre-check before walking at all.

**When the external change left conflicts, F7 owns the message.** The
Conflicts panel already appears on its own for exactly that state, and two
panels shouting about the same event is worse than one. The banner defers.

## Shipping with it: closing the stable-id contract

F7 found that `Document::ReplaceFeature` destroyed stable ids — the
`modify_feature` RPC sends parameters with no `"id"`, so every RPC edit reset
the feature's identity to 0, writing `features/0-box.json` and dangling every
reference to the old id. That is fixed. Two small things stop it recurring,
and they belong in this commit because both are "make an implied contract
enforceable":

- **Say it in the public surface.** `modify_feature`'s doc string
  (`RemoteModeling.cpp:127`) still reads "Replace the feature at `index` with
  `feature` JSON", which is the reading that caused the bug. It should say
  identity is preserved, and that naming a different `"id"` is how a caller
  says "this is a different feature now". That string is the `describe` row
  scripts and agents read.
- **Make id 0 unable to reach disk.** Every id space starts at 1, so only a
  carried-in id can be 0, and `SaveDir` builds filenames straight from ids. A
  save-time invariant across the eight id-keyed collections turns a whole
  class of corruption into a loud failure.

## Phases

- **D1 — the guard (~half a day).** `ProjectStamp`, `StampDiskState`, the
  check in `SaveProject`, the refusal dialog. This is the part that prevents
  data loss; everything else is comfort.
- **D2 — the banner (~2 hours).** `GLFW_FOCUSED` in the frame loop, the HEAD
  pre-check, the History-panel line, deferring to the Conflicts panel.
- **D3 — the stable-id invariant (~1 hour).** The two items above.

D1 + D3 ship together. D2 can follow.

## Open decisions (for Elias)

1. **Is "Save anyway" offered at all?** Reload-only is safer and simpler, and
   for a git project nothing is truly lost either way. But refusing to let
   someone overwrite their own project is the kind of paternalism that makes
   people work around the app.
2. **Does the stamp cover `geometry/`?** Excluded above, for consistency with
   F7's Derived class. The counter-argument is that a stamp of "the project
   directory" which skips part of the project directory is a rule to remember.
3. **Poll on focus only, or a slow timer too?** Focus-only misses a change
   made by a headless agent while the window stays focused — which is a real
   workflow here, not a hypothetical.

## Verification

- Unit: the stamp moves when a file is touched externally; does **not** move
  across a no-op save; does **not** move when our own save + commit runs;
  ignores `.git/` churn (write into `.git/`, stamp unchanged) and `geometry/`.
- Headless: open a project, `git checkout` another branch behind the app's
  back, call `save_project` → refused, with a reason that names the cause;
  then reload and the external state is what is in memory.
- GUI: the dialog appears on Ctrl+S after an external change; Reload picks the
  change up; Save anyway overwrites.
- The one that matters, as a regression test: **the terminal-panel scenario
  end to end** — move the working tree the way a shell command would, press
  Ctrl+S, and assert the external state survives and the user was told.

## Deferred, with reasons

- **A real file watcher (inotify / ReadDirectoryChangesW).** Platform-specific,
  needs a background thread, and needs to debounce the app's own writes — which
  is most of the complexity for a signal that focus + save-time checking
  already provides.
- **Merging in-memory changes against a moved disk.** F7's machinery is
  file-based; the in-memory document has no three-way base to merge against.
  Save first, then merge, is a coherent workflow. "Reload or overwrite" is the
  honest v1.
