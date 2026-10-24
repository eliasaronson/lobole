// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Autosaver.h"

#include "common/Logger.h"
#include "common/Platform.h"
#include "common/RuntimePaths.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace fs = std::filesystem;

std::string Autosaver::StateDir() {
  return RuntimePaths::UserStateDir().string();
}

namespace {

std::mutex g_tagMutex;
std::string g_tag; // "" until set: InstanceTag() then says "pid<pid>"

constexpr const char* kUnsavedPrefix = "unsaved-";
constexpr const char* kFileSuffix = ".autosave.json";
constexpr const char* kDirSuffix = ".autosave.d";

// The tag an unsaved slot's name carries; "" for the old shared slot.
std::string TagOf(const std::string& name) {
  std::string stem = name;
  for (const char* suffix : {kFileSuffix, kDirSuffix})
    if (stem.ends_with(suffix)) {
      stem.resize(stem.size() - std::char_traits<char>::length(suffix));
      break;
    }
  return stem.starts_with(kUnsavedPrefix)
             ? stem.substr(std::char_traits<char>::length(kUnsavedPrefix))
             : std::string();
}

} // namespace

void Autosaver::SetInstanceTag(const std::string& tag) {
  // A plain name: the tag becomes part of a file name in the state dir.
  if (tag.empty() || tag == "." || tag == ".." ||
      tag.find_first_of("/\\:") != std::string::npos) {
    LOG_WARN("Autosaver: refusing instance tag '{}'", tag);
    return;
  }
  std::lock_guard guard(g_tagMutex);
  g_tag = tag;
}

std::string Autosaver::InstanceTag() {
  std::lock_guard guard(g_tagMutex);
  return g_tag.empty() ? "pid" + std::to_string(platform::CurrentPid()) : g_tag;
}

bool Autosaver::IsUnsavedSlot(const std::string& name) {
  if (name == "unsaved.autosave.json" || name == "unsaved.autosave.d")
    return true;
  if (!name.starts_with(kUnsavedPrefix) ||
      !(name.ends_with(kFileSuffix) || name.ends_with(kDirSuffix)))
    return false;
  return !TagOf(name).empty();
}

void Autosaver::PruneUnsavedSlots(const std::string& stateDir,
                                  const std::vector<std::string>& live,
                                  size_t keep) {
  std::error_code ec;
  const std::string mine = InstanceTag();
  // Per tag, its file and directory together, aged by the newest of them.
  std::map<std::string, std::pair<fs::file_time_type, std::vector<fs::path>>>
      orphans;
  for (const auto& e : fs::directory_iterator(stateDir, ec)) {
    const std::string name = e.path().filename().string();
    if (!IsUnsavedSlot(name))
      continue;
    const std::string tag = TagOf(name);
    if (tag == mine || std::find(live.begin(), live.end(), tag) != live.end())
      continue;
    std::error_code tec;
    const fs::file_time_type t = fs::last_write_time(e.path(), tec);
    auto& [newest, paths] = orphans[tag];
    if (paths.empty() || t > newest)
      newest = t;
    paths.push_back(e.path());
  }
  std::vector<std::pair<fs::file_time_type, std::vector<fs::path>>> byAge;
  for (auto& [tag, entry] : orphans)
    byAge.push_back(std::move(entry));
  std::sort(byAge.begin(), byAge.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t i = keep; i < byAge.size(); ++i)
    for (const fs::path& p : byAge[i].second) {
      std::error_code rm;
      fs::remove_all(p, rm);
    }
}

std::string Autosaver::SidecarPath(const std::string& projectPath) {
  if (projectPath.empty())
    return (fs::path(StateDir()) /
            (kUnsavedPrefix + InstanceTag() + kFileSuffix))
        .string();
  return projectPath + ".autosave";
}

bool Autosaver::HasNewerAutosave(const std::string& projectPath) {
  std::error_code ec;
  const fs::path sidecar = SidecarPath(projectPath);
  if (!fs::exists(sidecar, ec))
    return false;
  if (projectPath.empty() || !fs::exists(projectPath, ec))
    return true;
  return fs::last_write_time(sidecar, ec) >
         fs::last_write_time(projectPath, ec);
}

void Autosaver::Clear(const std::string& projectPath) {
  std::error_code ec;
  fs::remove(SidecarPath(projectPath), ec);
  // A save means the disk is now the truth, for every document.
  fs::remove_all(SidecarDir(projectPath), ec);
}

std::string Autosaver::SidecarDir(const std::string& projectPath) {
  if (projectPath.empty())
    return (fs::path(StateDir()) /
            (kUnsavedPrefix + InstanceTag() + kDirSuffix))
        .string();
  return projectPath + ".autosave.d";
}

std::vector<std::string>
Autosaver::RecoverableDocuments(const std::string& projectPath) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path dir(SidecarDir(projectPath));
  if (!fs::is_directory(dir, ec))
    return out;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_regular_file(ec) || it->path().extension() != ".json")
      continue;
    // "<project>.autosave.d/parts/Stand.json" -> "parts/Stand", which is what
    // project.json calls that document.
    fs::path rel = fs::relative(it->path(), dir, ec);
    if (ec)
      continue;
    rel.replace_extension();
    out.push_back(rel.generic_string());
  }
  return out;
}

bool Autosaver::NeedsSave(uint64_t docId, uint64_t instanceId, uint64_t version,
                          uint64_t savedVersion) const {
  if (version == savedVersion)
    return false; // not dirty
  const auto it = m_saved.find(docId);
  if (it == m_saved.end())
    return true;
  return it->second.instanceId != instanceId || it->second.version != version;
}

void Autosaver::NoteSaved(uint64_t docId, uint64_t instanceId,
                          uint64_t version) {
  m_saved[docId] = Seen{instanceId, version};
}

bool Autosaver::MaybeSave(const Document& doc, const std::string& projectPath,
                          uint64_t savedVersion, double now,
                          std::string* error) {
  if (m_interval <= 0.0)
    return false;
  if (doc.SaveStamp() == savedVersion)
    return false; // not dirty
  if (doc.InstanceId() == m_lastSavedDocInstance &&
      doc.SaveStamp() == m_lastSavedDocVersion)
    return false; // unchanged since the last autosave
  if (now - m_lastSaveTime < m_interval)
    return false;

  const fs::path target = SidecarPath(projectPath);
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);

  // Atomic replace: a crash between Save and rename leaves the previous
  // sidecar intact; rename on the same filesystem is atomic.
  const fs::path tmp = target.string() + ".tmp";
  if (auto r = DocumentIO::Save(doc, tmp.string()); !r) {
    if (error)
      *error = r.error();
    return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    if (error)
      *error = "autosave rename failed: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }

  m_lastSaveTime = now;
  m_lastSavedDocInstance = doc.InstanceId();
  m_lastSavedDocVersion = doc.SaveStamp();
  LOG_INFO("Autosaved to {}", target.string());
  return true;
}
