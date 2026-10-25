# The disk poll off the frame

## The problem

On Windows the OM615 engine draws at 200+ fps and drops to a few fps every
few seconds. The likely cause is work the frame does on a timer:

- `PollDiskState` (src/ui/HistoryPanel.cpp, called from UI.cpp before the
  panels) asks every 3 s whether the project changed on disk. For a git
  project it first spawns `git` for the newest commit
  (`GitVault::Log(path, 1)`), then walks the whole project directory
  (`DocumentIO::StampProject`, src/core/io/ProjectStamp.cpp), reading every
  file's size and time. OM615's project is about 4 100 files: 41 ms warm on
  Linux, one dropped frame in three seconds and easy to miss. On Windows a
  process start and an NTFS stat cost several times more, and a scanner
  may open each file. That is the stutter's size and period.
- While the history panel is drawn, `RefreshRemotes` spawns `git` for the
  remotes and the ahead/behind counts every second (`kRefreshSeconds`).

Nothing about the question needs the frame: the banner must be right before
the next save, not this frame, and the save checks again for itself
(`ProjectChangedOnDisk` in the save and share paths, and the
`project_changed` RPC). Those stay synchronous: they guard a write, and a
write already blocks.

## The shape of the fix

**Audit first.** Every filesystem call and process spawn reached from the
frame on a timer or per frame, not from a user action: the two above, and
whatever else the audit finds (autosave's serialization, the instance
registry, settings flushes, the git status the panels show). Each is listed
in the area's doc with its cost on the OM615 project, measured with
`tools/profile.sh` or timed in place, and moved if it costs a frame.

**One background poller.** The periodic disk and git questions run on a
worker thread that owns its own copies of what it needs (the project path,
the stamp to compare against, the HEAD it last saw). The frame starts a poll
when the period has passed and none is running, and applies the answer when
one has arrived, setting `diskChanged`/`conflictsStale` exactly as today. A
poll started for one project whose answer arrives after the project changed
(opened, saved, closed) is discarded by comparing the project path and a
generation counter, never applied to the new one. Focus returning still
starts a poll at once; its answer still comes on a later frame.

**Cheaper where it is free.** The HEAD check reads `.git/HEAD` and the ref
it names (or `packed-refs`) instead of starting `git`; the walk skips
`.git/`, which `StampProject` should not be stamping (git's own writes are
not edits to the project). Both are checked against what `StampProject`
and `GitVault::Log` answer today on the same trees, so the banner fires on
exactly the same changes.

**The remotes refresh** moves onto the same worker, at the same period.

## Files

- `src/ui/HistoryPanel.cpp`, `src/ui/ProjectActions.cpp`, `src/ui/AppState.h`
  (the poller's state), a small poller beside them.
- `src/core/io/ProjectStamp.cpp`, `src/core/io/GitVault.{h,cpp}` (the HEAD
  read).
- Whatever else the audit moves.
- `docs/versioning/README.md` (external changes), `docs/architecture.md`
  (what may run on the frame).

## Verification

- A test hook makes the stamp slow (500 ms). A GUI test opens a project,
  lets the period pass, and asserts no frame waited on the stamp, then that
  an outside write raises the banner within a poll or two. It fails before
  the change (the frame waits the 500 ms).
- The existing external-change tests (F8: the banner, the refused save,
  share refused, `project_changed`) pass unchanged.
- A poll answer arriving after the project was switched is discarded (a
  unit test on the poller with a held answer).
- The HEAD read agrees with `git rev-parse HEAD` on a plain ref, a packed
  ref, a detached HEAD and an unborn branch.
- Measured on OM615: frame times over 30 s before and after, the longest
  frame quoted. Then the Windows build under Wine, and the user's brother's
  machine for the real answer.
- The headless suite, the GUI suite, `tools/ctest -L pygui`.
