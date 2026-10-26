# OpenCASCADE thread contention and the serial print plates

## Problem

**Exact clash checks get slower with more threads.** The strandbeest's exact
motion studies run their pair checks (`motion::ExactHit`, through
`Measure::BuildQuery`) on every core, and spend more time in the kernel than
computing: the leg-pair study on 22 threads took 148 s wall, 1105 s user and
1468 s system, against 84.6 s and 9 s system on 8 threads. perf puts the
time in `Standard_ErrorHandler`'s constructor and `Unlink`, each of which
takes one process-wide mutex.

Every `OCC_CATCH_SIGNALS` constructs a `Standard_ErrorHandler`, and on every
non-MSVC build OCCT defines `OCC_CONVERT_SIGNALS`, so the macro is live on
Linux and on our mingw Windows build. OCCT 7.6 to 7.9 keep the handlers of
ALL threads in one linked list behind one `Standard_Mutex`.
`GeomLib_CheckCurveOnSurface_TargetFunc::Value` opens one per function
evaluation, and every boolean's face/face step runs it thousands of times
(`IntTools_FaceFace::ComputeTolReached3d`). N threads of booleans queue on one
lock, and past a few threads they spend their time in futex calls. OCCT 8.0
made the list thread-local (OCCT#980). Ubuntu's apt OCCT, which CI builds against (7.6 on
24.04), does not have that change, and our overlay patches never reach it.

**The print plates run on one core.** On stage 2's plates, `check_geometry`,
`check_print` and `export_stl` cost about 225 s with CPU time equal to wall
time. Plate 4 has 123 solids: `export_stl` takes 64 s and `check_print` 55 s.

## Shape of the fix

1. **A kernel patch for the builds we compile OCCT for**
   (`nix/patches/occt-error-handler-thread-local.patch`; the nix overlay,
   the AppImage's `deps.sh` and the Windows cross build). The handler stack
   becomes a `thread_local` head with no mutex. That is only the `.cxx` half
   of OCCT#980: the header, the class layout and the exported symbols do not
   change. It is correct because a handler only ever belongs to the thread
   that made it. It lives on that thread's stack, `FindHandler` already
   filtered by thread id, and a signal converted to an exception is delivered
   to the thread that faulted (on Windows, the vectored handler runs on the
   faulting thread).

2. **A worker cap where the kernel still has the lock.** A build against an
   OCCT that keeps the global list (older than 8.0 and not carrying the
   patch: `OCC_VERSION_HEX` and `occpatch::Carries`) caps the threads a loop
   of booleans runs on. The cap is derived from the machine, not written as
   a literal; its form is set by the measured scaling on both kernels. Every
   `parallel::For` whose body is booleans asks for the same thing: the
   motion study's exact hits, the contact and interference sweep
   (`RunSweep`), the design sweep and the other-setup inspections. A loop of
   pure arithmetic or extrema keeps the whole machine. The rule lives next
   to `parallel::Workers()`, so a new boolean loop picks it up by asking for
   it.

3. **The plates, per solid.** First find where the serial time goes.
   `export_stl` fuses the bodies one at a time (122 chained fuses that grow
   each time) and meshes the result on one thread. `check_print` works per
   body. Parallelise what is independent per body with `core/Parallel.h`,
   only where it is safe under the OCC rules: a mesher writes triangulation
   onto the shape, so a worker meshes its own copy; booleans stay
   non-destructive. The output must be byte-identical to a serial run. STL
   triangles and every list in the report keep body order, merged by index
   after the workers finish. If a step is not safe or not worth it, it stays
   serial and the numbers say why.

## Files

- `nix/patches/occt-error-handler-thread-local.patch`, `nix/occt-overlay.nix`,
  `packaging/appimage/deps.sh`, `packaging/windows/occ-cross.nix`
- `src/core/Parallel.h` (or a `cad/` helper it calls) for the cap, and its
  callers: `src/core/MotionStudy.cpp`, `src/remote/RemoteInspection.cpp`
  (`RunSweep` only), `src/core/DesignSweep.cpp`,
  `src/core/cam/CamOtherSetups.cpp`
- `src/ui/StlExport.cpp`, `src/cad/CADKernel.cpp` (`SaveSTL`), and the print
  check's per-body work (`src/ui/PrintCheckActions.cpp`, `src/core/PrintCheck*`)
- tests: determinism of the parallel plate output against a one-thread run,
  and a CPU-time cost check where one is cheap
  (docs/ui/testing.md#tests-that-time-things)

## Verification

- Scaling on the leg-pair and walker studies (`recompute`), threads against
  wall and system time, before and after: on the patched nix OCCT, and on
  stock Ubuntu OCCT in a `tools/ci-local.sh` container where that is
  feasible.
- Plates 1 to 4: wall and CPU time of each call before and after, and the
  STL and the reports byte-identical to a serial run.
- `tools/test-fast.sh`, the GUI suite, `tools/ctest -L pygui -j1`,
  `tools/ci-local.sh --containers --image ubuntu:24.04` and
  `tools/ci-local.sh --asan`.
- `generator_strandbeest` alone, against its ~1292 s on main.
