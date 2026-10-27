// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/RecentProjects.h"

#include "core/io/Autosaver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace RecentProjects {
namespace {

// Same path for the same project however it was spelled — otherwise
// "./bracket.lob" and an absolute path are two entries for one file, and
// the list fills up with the same project.
std::string Canonical(const std::string& path) {
  std::error_code ec;
  const fs::path abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
  return ec ? path : abs.string();
}

std::vector<std::string> Read() {
  std::vector<std::string> out;
  std::ifstream in(FilePath());
  if (!in)
    return out;
  try {
    const json j = json::parse(in);
    for (const json& e : j.value("projects", json::array())) {
      if (e.is_string())
        out.push_back(e.get<std::string>());
    }
  } catch (const std::exception&) {
    // A corrupt list is worth exactly nothing: start over rather than
    // refuse to open projects.
    return {};
  }
  if (out.size() > kMax)
    out.resize(kMax);
  return out;
}

void Write(const std::vector<std::string>& paths) {
  try {
    const fs::path target(FilePath());
    fs::create_directories(target.parent_path());
    const fs::path tmp = target.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << json{{"projects", paths}}.dump(2) << "\n";
      if (!out)
        return;
    }
    fs::rename(tmp, target);
  } catch (const std::exception&) {
    // Losing the recent list costs a menu, never a document.
  }
}

} // namespace

std::string FilePath() {
  return (fs::path(Autosaver::StateDir()) / "recent-projects.json").string();
}

void Remember(const std::string& path) {
  if (path.empty())
    return;
  const std::string canonical = Canonical(path);
  std::vector<std::string> paths = Read();
  paths.erase(std::remove_if(paths.begin(), paths.end(),
                             [&](const std::string& p) {
                               return Canonical(p) == canonical;
                             }),
              paths.end());
  paths.insert(paths.begin(), canonical);
  if (paths.size() > kMax)
    paths.resize(kMax);
  Write(paths);
}

std::vector<std::string> List() { return Read(); }

void Forget(const std::string& path) {
  const std::string canonical = Canonical(path);
  std::vector<std::string> paths = Read();
  paths.erase(std::remove_if(paths.begin(), paths.end(),
                             [&](const std::string& p) {
                               return Canonical(p) == canonical;
                             }),
              paths.end());
  Write(paths);
}

void Clear() { Write({}); }

} // namespace RecentProjects
