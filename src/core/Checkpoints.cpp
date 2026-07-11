#include "core/Checkpoints.h"

#include "Logger.h"
#include "core/Serialization.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace Checkpoints {

namespace {

bool ValidName(const std::string& name) {
  return !name.empty() && name.find('/') == std::string::npos &&
         name.find('\\') == std::string::npos && name != "." &&
         name != ".." && name.size() <= 128;
}

int64_t MTimeUnix(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (ec)
    return 0;
  return std::chrono::duration_cast<std::chrono::seconds>(
             t.time_since_epoch() -
             fs::file_time_type::clock::now().time_since_epoch() +
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

std::string Dir(const std::string& projectPath) {
  return projectPath.empty() ? std::string{} : projectPath + ".checkpoints";
}

std::string PathOf(const std::string& projectPath, const std::string& name) {
  if (projectPath.empty() || !ValidName(name))
    return {};
  return (fs::path(Dir(projectPath)) / (name + ".json")).string();
}

bool Create(const Document& doc, const std::string& projectPath,
            const std::string& name, std::string* error) {
  if (projectPath.empty()) {
    if (error)
      *error = "save the project first — checkpoints live beside it";
    return false;
  }
  if (!ValidName(name)) {
    if (error)
      *error = "invalid checkpoint name (no path separators, max 128 chars)";
    return false;
  }
  std::error_code ec;
  fs::create_directories(Dir(projectPath), ec);
  const std::string target = PathOf(projectPath, name);
  if (!DocumentIO::Save(doc, target, error))
    return false;
  LOG_INFO("Checkpoint '{}' created at {}", name, target);
  return true;
}

std::vector<Info> List(const std::string& projectPath) {
  std::vector<Info> out;
  if (projectPath.empty())
    return out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(Dir(projectPath), ec)) {
    if (entry.path().extension() != ".json")
      continue;
    out.push_back({entry.path().stem().string(), MTimeUnix(entry.path())});
  }
  std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
    return a.modifiedUnix > b.modifiedUnix;
  });
  return out;
}

bool Remove(const std::string& projectPath, const std::string& name) {
  const std::string path = PathOf(projectPath, name);
  if (path.empty())
    return false;
  std::error_code ec;
  return fs::remove(path, ec);
}

} // namespace Checkpoints
