# CAM speed and the GUI's loose ends

What the chess round and the CAM GUI round left, in groups that touch
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

## 4. Two bugs the GUI round found

- **Editor field edits do not undo.** The op, tool and fixture editors in the
  tool window draw under no undo guard, so a changed depth, tool or fixture
  size cannot be taken back (the fixture editor's explicit snapshots aside).
  Each committed field edit should be one undo step, as the setup editor's
  are.
- **A setup's `rapidFeed` is not saved.** Only the machine's copy is, so a
  setup's own rapid rate is lost on reload. It goes in the setup's JSON, which
  is a format bump.

**Verified by:** a GUI test per editor (edit a field, undo, the value is
back), and a golden round-trip of a setup's rapid feed.

