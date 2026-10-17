# Requirement checks off the frame, and the engine's failing requirement

## Problem

1. **A check holds the frame.** The `requirements` job has no worker: its
   `finish` measures one requirement a frame ON the main thread. A clearance
   over a motion study (the engine's valve-to-piston gaps: 181 samples, each
   an exact `ContactBetween`) is seconds to minutes in one `finish`, so the
   watchdog logs stalls of 7-216 s while the panel's progress bar and Cancel
   are frozen with it. A synchronous `check_requirements` over the RPC runs
   the same measures inside the handler, on the frame too.
2. **The engine's "turns clean" fails in the app and passes in the
   generator.** The generator hides the timing belt, runs the 720° study and
   checks its requirements, then shows the belt again and saves. A motion
   study skips hidden occurrences and its input key reads every occurrence's
   `visible`, so the saved file opens with every requirement stale; Check
   re-runs the study with the belt in it, and a belt that is a rigid ring
   meshes its pulleys only at whole teeth, so it clashes at 144 of the 181
   samples. The requirement was checked against a state the document does
   not keep. The failed verdict also names nothing: `detail` holds counts
   only, so the panel says "findings" with no finding.

## The fix

**A measure's expensive half runs on a worker.** `MeasureClearance` reads
everything on the main thread (both sides' bodies as deep copies,
`ContactBody`'s way, and every sample's pose pair) and hands back a `work`
closure that owns only those copies. In the job, `StepCheck` starts it on a
thread the `CheckRun` owns and polls it each frame; the main thread only
turns the reading into evidence. The evidence's input hash is the one taken
when the copies were made, so an edit while it measures reads `stale` (not
a pass for a model nobody measured), a requirement edited meanwhile
(revision moved) or removed gets nothing, and Cancel sets the worker's flag,
which it reads between samples; the job ends when the worker has joined.
Synchronous callers (`verify_assertions`, a `batch`) run the same closure
inline.

**The RPC parks.** A synchronous `check_requirements` that has anything to
measure starts the same job (owned by its connection) and parks
(`RemoteParking.h`) until it is reaped, then answers with exactly what the
synchronous form answered: rows, counts, `checked`, `reused`, receipt.

**A prefilter makes the exact clearance cheap.** Per sample, the meshes
bound the exact gap from below (mesh distance less both deflections). The
samples are measured exactly in order of that bound, and the walk stops when
the next bound is no smaller than the best exact gap found: the same minimum,
at the same sample, from the few samples that can hold it. `detail` says how
many were measured exactly.

**The engine states what it checks.** The generator stops toggling the belt:
the 720° study runs under an assembly configuration that suppresses the belt
(saved in the document, so the app runs the same study), the belt study
becomes a requirement of its own ("the belt sits in its pulleys at every
tooth", over its tooth-step study), the requirements are checked last, after
the controls that move the assembly, and the generator reopens the saved file
and fails unless every requirement reads `pass` without measuring.
`motion_clean`'s detail names the clashing pairs (with the first driver value
each clashes at), the unsolved samples and the branch jumps as `findings`.

## Files

- `src/remote/RemoteRequirements.{h,cpp}` — the worker, the evidence on
  landing, the prefilter, the parking, motion_clean findings.
- `src/remote/RemoteUiActions.cpp` — asks the requirements domain to park;
  sets the caller's connection while parking so a parked job has an owner.
- `generators/engine/build.py` — the configuration, the belt requirement,
  the order, the reopen.
- `tests/requirements_smoke.py` — a check under a held worker answers while
  the frame runs; Cancel lands; an edit mid-check reads stale.
- a GUI test under `requirements/` — a slow check keeps frames coming and
  Cancel works.
- docs: `docs/requirements/README.md` (checking is a job), and the engine's
  README.

## Verification

- The engine's valve-to-piston requirements: same values before and after,
  time before and after.
- `tools/ctest -LE gui`, `./tools/gui-tests-xvfb.sh`, `generator_engine`.
