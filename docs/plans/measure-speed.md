# Fast exact distance and a contact report that does not block

## The problem

**Exact distance falls back too readily.** Exact distance between two lofted
gears takes about 20 s a pair, and the planetary generator spends two of its
four minutes on six such pairs. `ExactDistance` (src/core/Measure.cpp)
already prunes with a mesh pre-pass, but gives up when more than 64
candidate face pairs are close (`kMaxRefinedPairs`). It then hands the whole
shapes to `BRepExtrema_DistShapeShape`. Meshing teeth are many nearly
parallel faces, so they always trip it. Element-level distance
(`DistanceDetailed`) does no pruning at all and asks for MINMAX.

**`contact_report` blocks the instance.** It is synchronous. On an
assembly of five lofted gears it ran past a minute and blocked the
instance. Every pair whose boxes overlap pays an exact boolean, and a
running boolean cannot be interrupted, so `deadline_s` only stops new
pairs from starting.

## The shape of the fix

**`ExactDistance`:**

- Refine only the candidate face pairs whose triangle distance is within
  the best mesh distance plus twice the deflection, rather than all of
  them or none.
- Run those pairs in parallel (`parallel::For`; each pair is independent,
  and per docs/inspect/README.md#query-booleans-are-non-destructive
  extrema write nothing onto the shapes).
- Fall back to the whole shapes only if that narrowed set is still large.
- `DistanceDetailed` goes through the same path with
  `Extrema_ExtFlag_MIN`.

**`contact_report`:**

- **`mode: "mesh"`** screens every pair on the tessellation, as the
  interference sweep already can, and runs the exact boolean only on pairs
  the mesh says intersect or touch.
- **`async: true`** runs it as a job (`contact` in `background_jobs`,
  polled with a status RPC, `outlive_client` as the study jobs have). Its
  compute already touches only its own shapes.

## UI

None. `contact_report` and exact distance have no GUI today. The Measure
tool's distance takes the faster path without a visible change, other than
answering sooner. An async `contact_report` is visible as a `contact` entry
in `background_jobs`, and there is no global jobs window in which it would
appear.

## Files

- `src/core/Measure.{h,cpp}`: the narrowed refinement, parallel pairs,
  `DistanceDetailed`.
- `src/remote/RemoteInspection.cpp`: `contact_report`'s mode, async and
  status rows.
- `src/ui/` background job plumbing (as `RemoteMotion.cpp`'s job does).
- `tests/test_measure*.cpp`, an inspection smoke, docs/inspect/README.md,
  docs/scripting/rpc.md.

## Verification

- **Same answers, before and after:**
  - the existing distance tests;
  - a pair of the planetary's gears;
  - the propeller pair the code comment records (177 candidates).
- **Speed, measured before and after:** a gear pair, and the planetary
  generator's clearance step. The tests assert the answers, not the
  timings.
- **contact_report:** a mesh-mode run on a meshing pair names the same
  clashes as the exact one. An async run reports progress, can be
  cancelled, and leaves the instance answering other calls meanwhile.
