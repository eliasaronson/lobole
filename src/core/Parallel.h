// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Coarse-grained parallelism for the work that is actually worth it: batches
// of INDEPENDENT OpenCASCADE booleans. A clash check is ~161 ms on an engine
// part, and a 49-sample motion study on a 241-occurrence assembly is 20,403
// of them — 3292 s on one core.
//
// Deliberately not a general task system. These loops are flat, their bodies
// are long, and the only thing they need is "run these n things on every
// core and tell me when they are done".
//
// THE CAVEAT THAT MATTERS: OpenCASCADE fills caches lazily ON the shape
// (bounding boxes, tolerances, triangulation), and in an assembly one body
// appears in many pairs. Concurrent work over shared shapes can therefore
// race even when nothing "modifies" anything. Every caller here warms those
// caches single-threaded first — and `tests/sweep_parallel_smoke.py` diffs a
// one-thread report against an all-thread one, byte for byte, because a race
// in a boolean corrupts an ANSWER rather than crashing.
//
// AND THE ONE WARMING CANNOT FIX: a boolean does not only READ its arguments,
// it WRITES to them — the p-curves it computes go onto the input edges unless
// SetNonDestructive says otherwise. There is nothing to pre-warm, because
// what gets written is specific to the PAIR: no single-threaded pass can make
// A-with-B and A-with-C agree about what belongs on A's edges. That one
// corrupts the HEAP rather than an answer, so the byte-diff above does not
// see it either; it cost three runs in ten of one smoke for a day
// (docs/inspect/README.md#query-booleans-are-non-destructive). Anything a
// worker here calls must leave the shapes alone, not merely find them warm.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

namespace parallel {

// How many threads the heavy loops may use. DESIGNER_SWEEP_THREADS pins it:
// the gate sets it to 1 so a parallel answer can be diffed against a serial
// one, and it is the escape hatch if OpenCASCADE ever misbehaves under
// concurrency on somebody's machine.
//
// Inside a `For`, it is this worker's SHARE of them: a loop nested in
// another's body (the other setups of a job inspected at once, each
// inspection itself split into bands) divides the machine between the outer
// loop's workers rather than each of them starting a machine's worth of
// threads.
namespace detail {
inline thread_local size_t tlShare = 0; // 0: not inside a For
} // namespace detail

inline size_t MachineWorkers() {
  if (const char* env = std::getenv("DESIGNER_SWEEP_THREADS")) {
    const int want = std::atoi(env);
    if (want > 0)
      return static_cast<size_t>(want);
  }
  return std::max(1u, std::thread::hardware_concurrency());
}

inline size_t Workers() {
  return detail::tlShare > 0 ? detail::tlShare : MachineWorkers();
}

// Calls `body(k)` for every k in [0, n). No ordering guarantee, so `body`
// must touch nothing shared — collect into `results[k]` and merge afterwards
// in k order, which is what keeps a response reproducible.
template <typename F> void For(size_t n, F&& body) {
  if (n == 0)
    return;
  std::atomic<size_t> next{0};
  const size_t budget = Workers();
  const size_t threads = std::min<size_t>(n, budget);
  // What a For nested in `body` may use: this loop's budget split between
  // its threads, the remainder going to the first ones.
  const auto shareOf = [&](size_t t) {
    return std::max<size_t>(1, budget / threads + (t < budget % threads));
  };
  const auto worker = [&](size_t t) {
    // Restored however `body` leaves, a throw included.
    struct Share {
      size_t was = detail::tlShare;
      explicit Share(size_t now) { detail::tlShare = now; }
      ~Share() { detail::tlShare = was; }
    } share(shareOf(t));
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= n)
        return;
      body(k);
    }
  };
  if (threads <= 1) {
    worker(0);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (size_t t = 1; t < threads; ++t)
    pool.emplace_back(worker, t);
  worker(0); // this thread pulls its share rather than idling
  for (std::thread& t : pool)
    t.join();
}

} // namespace parallel
