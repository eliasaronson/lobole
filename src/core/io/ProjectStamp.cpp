#include "core/io/ProjectStamp.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace DocumentIO {

namespace {

uint64_t MtimeTicks(const fs::directory_entry& entry, std::error_code& ec) {
  const auto time = entry.last_write_time(ec);
  if (ec)
    return 0;
  return static_cast<uint64_t>(time.time_since_epoch().count());
}

} // namespace

ProjectStamp StampProject(const std::string& path) {
  ProjectStamp stamp;
  if (path.empty())
    return stamp;

  std::error_code ec;
  const fs::path root(path);
  if (!fs::exists(root, ec) || ec)
    return stamp;

  // A legacy single-file project is its own stamp.
  if (!fs::is_directory(root, ec)) {
    const fs::directory_entry entry(root, ec);
    if (ec)
      return stamp;
    stamp.fileCount = 1;
    stamp.totalSize = entry.file_size(ec);
    stamp.maxMtime = MtimeTicks(entry, ec);
    stamp.valid = true;
    return stamp;
  }

  auto it = fs::recursive_directory_iterator(
      root, fs::directory_options::skip_permission_denied, ec);
  if (ec)
    return stamp;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      // Skipped by name at any depth: `geometry/` sits beside each document
      // in a multi-document project, not only at the root.
      if (name == ".git" || name == "geometry")
        it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec))
      continue;
    stamp.fileCount++;
    stamp.totalSize += it->file_size(ec);
    stamp.maxMtime = std::max(stamp.maxMtime, MtimeTicks(*it, ec));
  }
  stamp.valid = true;
  return stamp;
}

} // namespace DocumentIO
