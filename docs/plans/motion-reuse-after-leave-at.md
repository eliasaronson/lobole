# A motion study's result survives its own `leave_at`

## The problem

`run_motion_study` answers from a stored run when its inputs hash the same
(`motion::InputKey`, `src/core/MotionStudy.cpp`). The hash includes every
occurrence's placement and every mate's current value, because a run
starts from the pose the document is in: `WalkToStart` walks the driver
from there to the first sample, and which branch a linkage lands on
depends on where that walk began. So the start pose is a genuine input,
and a scrub or a drag onto the other branch must run the study again.

`leave_at` (and `leave_at_deg` / `leave_at_mm`) then poses the live
assembly at one of the run's samples (`ui::ApplyMotionSample`). That moves
the placements and mate values the hash reads, so the next
`run_motion_study {leave_at: j}` hashes differently, misses, and runs the
whole study again -- and so does every one after it. Even
`leave_at: "start"` does not bring the hash back, since sample 0 is the
first sample, not the pose the first run started from. A caller measuring
something at a dozen samples pays a dozen full runs: on the Strandbeest
generator (`check_layer_clearance`) that is ~1220 s of a 2314 s build,
~99 s a call on the walker. The stored result also reads `stale` in
`motion_status` / `list_motion_studies` after a `leave_at`, and a project
saved there reopens with it stale.

## The fix

Keep the start pose an input, and let `leave_at` record what it did. A
pose written in from the result's own samples is a pose on the path the
result describes: walking from it to the start retraces that path, so the
result is the study's answer from there as much as from where the run
began. After `ApplyMotionSample` poses the assembly (both the sample and
the value forms go through it), it re-keys the study's installed result:

- the hash of the posed assembly (`motion::InputKey` again, after the one
  solve) becomes the result's `inputHash`, and the result is kept in
  `motionResultsByInput` under it as well -- the hash it was run for stays
  there too, so an undo back to the original pose is still answered;
- the stored revision is taken again, so the result is not `stale`;
- the copy the assembly carries into the project gets the new hash, so a
  project saved after `leave_at` reopens with its result current.

Only the study that was left re-keys, and only when the result being
posed from is the one installed for it (same `inputHash`). Everything else
is unchanged: a scrub (`set_motion_pose`), a drag, a moved fixed
occurrence, a changed non-driven mate, a part edit or a configuration
switch afterwards still hash differently and run again. Another study on
the same assembly is not re-keyed: `leave_at` of one study genuinely moved
the other's start pose.

Files: `src/ui/MotionActions.cpp` (the re-key at the end of
`ApplyMotionSample`), `src/remote/RemoteMotion.cpp` (the
`run_motion_study` describe row's reuse text).

## Verification

- `tests/study_jobs_smoke.py`: after a run, `leave_at` at a middle sample,
  then `leave_at` at another sample, `leave_at_deg`, and `"start"`: every
  call `reused: true` and `motion_status` not stale; then a real edit (a
  mate value set by hand) runs again, `reused: false`. Fails before the
  fix (the second call re-runs).
- `tools/test-fast.sh`, the GUI suite, `tools/ctest -L pygui -j1`.
- `generator_strandbeest` alone: its wall time drops by about the 1220 s
  the re-runs cost.
