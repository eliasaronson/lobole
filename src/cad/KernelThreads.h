// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// How many threads a loop of OpenCASCADE booleans may run on, where the
// kernel itself makes more threads slower.
//
// Every OCC_CATCH_SIGNALS scope links a Standard_ErrorHandler into a stack.
// OCCT 7.6 to 7.9 keep ONE stack for every thread, behind one mutex, and a
// boolean opens thousands of scopes: two per function evaluation of the
// curve-on-surface check every face/face intersection runs
// (GeomLib_CheckCurveOnSurface, from IntTools_FaceFace::ComputeTolReached3d).
// Past a dozen threads the workers queue on that mutex: the strandbeest's
// leg-pair clash study took 68 s on 11 threads and 94 to 139 s on 22. OCCT
// 8.0 keeps the stack per thread (OCCT#980), and so does every kernel we
// build (nix/patches/occt-error-handler-thread-local.patch); a stock
// distribution OCCT older than 8.0 does not
// (docs/performance/README.md#the-kernels-error-handler-lock).
//
// On a shared stack a loop of booleans runs on half the hardware threads.
// Where the queue starts depends on the geometry -- two crossed cylinders
// queue from two threads, the leg pair from about sixteen -- so no count is
// right for every loop. Half is what the measurements chose: on 22 hardware
// threads it took the leg pair from 94-139 s to 64 s and the walker study
// from 478 s to 344 s; on 8 (four cores, where nothing queued) it cost the
// leg pair 112 s against 99 s, the second thread of a core buying little.
//
// Only the thread count moves: every item is still done once, and every
// result is what a one-thread run gives (core/Parallel.h).

#include <cstddef>

namespace cad {

// True when this OpenCASCADE keeps its exception handlers per thread: OCCT
// 8.0 and later, or one carrying occt-error-handler-thread-local.patch.
// Asked of the running library, once.
bool PerThreadErrorHandlers();

// How many of `budget` threads a loop whose items are booleans may use:
// all of them on a per-thread stack or with LOBOLE_SWEEP_THREADS pinned,
// at most half the machine's hardware threads on a shared one.
size_t BooleanWorkers(size_t budget);

// The rule alone, for its test.
size_t BooleanWorkers(size_t budget, size_t hardwareThreads, bool perThread);

} // namespace cad
