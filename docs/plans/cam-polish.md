# CAM speed and the GUI's loose ends

What the chess round and the CAM GUI round left, in three groups that touch
different files.

## 1. Turned blanks generate faster

The four turning setups are 78% of the chess CAM run. The knights blank's
toolpaths took 283 s cold: 1.2 million moves over three rotary passes, each
placed by dropping the cutter onto the triangulated part. Verifying took
55 s. A second generate in the same session takes 0.2 s from the in-memory
cache, but nothing survives a restart or a new run.

- **Profile first** (`tools/profile.sh --app`, one knights generate). This
  shows where the time goes: CL-surface sampling, the drop-cutter kernel,
  pruning, or the move emission. It also shows whether it already uses every
  core.
- **Parallelize** the sampling across rows of the CL grid if it is serial
  (docs/architecture.md on OCC and threads: this is triangle math, not OCC
  booleans, so it is safe to split).
- **Keep generated passes on disk.** The surface cache keyed on `SurfaceKey`
  (which hashes every input the pass reads, `FramingKey` included) is the
  right key for a file under `$XDG_CACHE_HOME/designer/cam/`. A rerun of an
  unchanged job, or a restart, then reads instead of recomputing. Bounded in
  size, oldest out.
- **Rough on the grid the rough needs.** The rough pass is sampled as finely
  as the finish. Its stepdown and stock-to-leave set a coarser useful grid.
  Derive it from those, never a literal, and verify the rough still leaves
  an even skin for the finish.

**Verified by:** the knights generate time before and after, the same verdict
(no gouge, same leftover), `ctest -L cam`, and the chess set's CAM stage time.

## 2. The CAM GUI

- **The Engrave verb's window grows over the viewport** with its verdict and
  covers the curves you are trying to click. It should keep a fixed height
  and scroll, as the other verb windows do.
- **The job tab's 3D view was badly framed** in every agent's screenshot.
  Reproduce it, find why, and frame to the job's parts and stock.
- **Delete** does not reach setups, parts, fixtures or tools, only their
  menus do. They get `SelectionItem` kinds, so the key and the list's
  multi-select work as they do for features.
- **Returning to a job at a non-quarter index** pays its exact bounding boxes
  again on every visit (about 210 ms on the frame-time test's part, most of a
  second on the chess blanks). `CachedStockView` and `JobPlacedBodies` hold
  one job at a time. They should hold one entry per job, or the stock view
  should reuse `JobPlacedBodies`' already-warm copies instead of framing
  again. The first visit's one-off cost (0.8 to 1.4 s on a rotary blank)
  moves to a background warm when the project opens.
- **A part row's tail** shows the material slug ("wood_oak"); it should show
  the material's name.
- **Deleting an op or a tool** may record two undo steps, because
  `RemoveCamOperations` and `RemoveCamTool` snapshot inside the panel's open
  guard. Test it, and fix it if so.
- **The CAM panel serializes the whole job every frame** to detect edits
  (`JobUndoGuard` → `JobFingerprint`, about 8% of an idle frame). A version
  counter the job's mutators bump replaces it.
- **Engrave ops are missing from the reach window** (`DeriveReachOffset`).

**Verified by:** GUI tests for Delete on each kind, the engrave window's
height, the job framing, and undo counts; the frame-time test's bound on the
non-quarter revisit tightened to what it now measures; screenshots.

## 3. Build hygiene

- **A true Debug build (`-Og`) prints 9 warnings**, among them
  `-Wformat-truncation` in `CurveFeature.cpp` and `-Wunused-result` in
  `test_gitvault.cpp`. Fix them.
- **`gui-tests-xvfb.sh` and `worktree-build.sh` enter `nix-shell` directly**;
  `tools/nix-run` saves ~5 s each time.

**Verified by:** a Debug build with no warnings, and both scripts' runs.
