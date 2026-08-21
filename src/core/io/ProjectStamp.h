#pragma once

#include <cstdint>
#include <string>

namespace DocumentIO {

// Has the project on disk changed since we last read or wrote it? (F8)
//
// A project is a directory and the app holds its documents in memory, but
// nothing compared the two: `ProjectDirty` weighs in-memory document
// versions, and the only filesystem comparison anywhere was the autosave
// sidecar's mtime. So an external write — a `git checkout` in the terminal
// panel that ships inside the app, another editor, a second instance — was
// silently overwritten by the next save.
//
// A quick check, not a content hash: hashing every `.brep` on every save
// would cost far more than it buys. Count + size + newest mtime is what rsync
// calls a quick check, and it catches what actually happens — git writes
// files with the CURRENT mtime on checkout, merge, restore and pull, so even
// putting an OLDER tree on disk moves this stamp.
//
// What it cannot catch, stated rather than glossed: a write that lands in the
// same filesystem mtime tick AND preserves the exact byte count. Coarse mtime
// granularity or clock skew on a network filesystem widens that window — the
// check degrades to "usually notices", never to "wrong".
struct ProjectStamp {
  uint64_t fileCount = 0;
  uint64_t maxMtime = 0; // ticks of the filesystem clock, not wall time
  uint64_t totalSize = 0;
  // False when nothing was stamped (no project yet, or the path is gone).
  // An invalid stamp never compares equal to anything, including itself, so
  // "we have no idea" can never be mistaken for "unchanged".
  bool valid = false;

  bool Matches(const ProjectStamp& other) const {
    return valid && other.valid && fileCount == other.fileCount &&
           maxMtime == other.maxMtime && totalSize == other.totalSize;
  }
};

// Stamp the project at `path` — a `.dsgn` directory, or a legacy single file.
//
// Two directories are skipped, both on purpose:
//   .git/      is not the document, and it churns on its own — the index, the
//              reflog, and detached `maintenance`/`gc --auto` runs that write
//              while you are reading.
//   geometry/  is the BUILD, not the recipe. F7 settled that baked geometry
//              is derived data with no meaningful conflict (the chain hash
//              decides at load), so a guard that fired on it would contradict
//              that. The stamp covers the recipe.
ProjectStamp StampProject(const std::string& path);

} // namespace DocumentIO
