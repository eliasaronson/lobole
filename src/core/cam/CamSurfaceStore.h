#pragma once

#include "core/Feature.h"        // Body
#include "core/cam/CamSurface.h" // SurfaceResult

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

// Surfacing passes kept ON DISK between runs
// (docs/ui/long-operations.md#passes-kept-on-disk).
//
// A turning's passes are minutes each -- the knights blank's rough, finish
// and rest took three -- and the in-memory `SurfaceCache` is gone the moment
// the app is. So a finished pass is also written here, and a pass the memory
// cache misses is read back before it is cut again: a restart, or a rerun of
// an unchanged job, reads instead of recomputing.
//
// The key is `SurfaceKey` with the geometry fingerprint taken from the
// bodies THEMSELVES (`BodiesContentKey`), not the caller's `bodiesKey`: that
// one is made of version counters, which mean "unchanged since" inside one
// session and nothing across two -- two documents opened in two runs can
// share every counter and not one face. And every file carries a SALT
// naming the build that wrote it, because the key covers the inputs and not
// the code: a pass cut by yesterday's generator is not today's answer.
namespace cam {

// A fingerprint of the bodies' geometry: each shape's exact B-rep (no
// triangulation), and the names and ids a pick resolves through. Equal
// shapes give equal keys in any run; any edit to a face changes it.
[[nodiscard]] uint64_t BodiesContentKey(const std::vector<Body>& bodies);

class SurfaceStore {
public:
  // `dir` is made on the first write. `maxBytes` bounds what is kept: after
  // a write, the least recently used files go until the rest fit. `salt` is
  // asked once, on first use -- the app's hashes its own executable, which
  // is not free, and a session that never cuts a surface never pays it.
  SurfaceStore(std::filesystem::path dir, uint64_t maxBytes,
               std::function<uint64_t()> salt);

  // The pass stored under `key` by this build, or false. A file that is
  // torn, from another build or for another key is a miss, and is removed.
  // A hit is touched, so eviction takes the ones nobody reads.
  bool Load(uint64_t key, SurfaceResult& out) const;
  // Written whole to a temporary and renamed, so a reader never sees half a
  // file; then the store is trimmed to `maxBytes`. Failure is silent: the
  // store only ever saves time.
  void Save(uint64_t key, const SurfaceResult& result) const;

  [[nodiscard]] const std::filesystem::path& Dir() const { return m_dir; }

private:
  [[nodiscard]] uint64_t Salt() const;
  [[nodiscard]] std::filesystem::path PathOf(uint64_t key) const;
  void Trim() const;

  std::filesystem::path m_dir;
  uint64_t m_maxBytes = 0;
  std::function<uint64_t()> m_saltFn;
  mutable std::once_flag m_saltOnce;
  mutable uint64_t m_salt = 0;
  mutable std::mutex m_trim; // a worker and the frame may both write
};

// One surfacing operation, read from `store` when it holds it, else cut and
// written to it. `store` may be null, which is `SurfacePass`.
[[nodiscard]] SurfaceResult
SurfacePassStored(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies, const SurfaceStore* store);

} // namespace cam
