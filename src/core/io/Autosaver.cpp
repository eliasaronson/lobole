// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Autosaver.h"

#include "common/Logger.h"
#include "common/Platform.h"
#include "common/RuntimePaths.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace fs = std::filesystem;

std::string Autosaver::StateDir() {
  return RuntimePaths::UserStateDir().string();
}

namespace {

std::mutex g_tagMutex;
std::string g_tag;  // "" until set: InstanceTag() then says "pid<pid>"
std::string g_kind; // "" = no record of this instance's kind

constexpr const char* kUnsavedPrefix = "unsaved-";
constexpr const char* kFileSuffix = ".autosave.json";
constexpr const char* kDirSuffix = ".autosave.d";
// Beside a slot, one word: the kind of instance that wrote it. A sidecar of
// its own because the slot is a document (or a directory of them) in the
// document format, which has no place for who wrote it.
constexpr const char* kKindSuffix = ".autosave.kind";

fs::path KindPath(const fs::path& stateDir, const std::string& tag) {
  return stateDir / (kUnsavedPrefix + tag + kKindSuffix);
}

std::string ReadKind(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec))
    return {};
  std::ifstream in(file);
  std::string word;
  in >> word;
  return word;
}

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

void Autosaver::SetInstanceKind(const std::string& kind) {
  if (kind.find_first_of(" \t\r\n/\\") != std::string::npos) {
    LOG_WARN("Autosaver: refusing instance kind '{}'", kind);
    return;
  }
  std::lock_guard guard(g_tagMutex);
  g_kind = kind;
}

std::string Autosaver::InstanceKind() {
  std::lock_guard guard(g_tagMutex);
  return g_kind;
}

bool Autosaver::OfferedToPerson(const UnsavedSlot& slot) {
  return slot.kind.empty() || slot.kind == "windowed";
}

void Autosaver::NoteSlotKind() {
  const std::string kind = InstanceKind();
  if (kind.empty())
    return;
  const fs::path file = KindPath(StateDir(), InstanceTag());
  if (ReadKind(file) == kind)
    return;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << kind << "\n";
    if (!out) {
      LOG_WARN("Autosaver: could not write {}", tmp.string());
      return;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    LOG_WARN("Autosaver: could not write {}: {}", file.string(), ec.message());
    fs::remove(tmp, ec);
  }
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
  std::vector<std::pair<std::string, fs::path>> kinds;
  std::set<std::string> slotted;
  for (const auto& e : fs::directory_iterator(stateDir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.starts_with(kUnsavedPrefix) && name.ends_with(kKindSuffix)) {
      kinds.emplace_back(
          name.substr(std::char_traits<char>::length(kUnsavedPrefix),
                      name.size() -
                          std::char_traits<char>::length(kUnsavedPrefix) -
                          std::char_traits<char>::length(kKindSuffix)),
          e.path());
      continue;
    }
    if (!IsUnsavedSlot(name))
      continue;
    const std::string tag = TagOf(name);
    slotted.insert(tag);
    if (tag == mine || std::find(live.begin(), live.end(), tag) != live.end())
      continue;
    std::error_code tec;
    const fs::file_time_type t = fs::last_write_time(e.path(), tec);
    auto& [newest, paths] = orphans[tag];
    if (paths.empty()) {
      newest = t;
      paths.push_back(KindPath(stateDir, tag)); // goes with its slot
    } else if (t > newest) {
      newest = t;
    }
    paths.push_back(e.path());
  }
  // A record whose slot is gone (cleared by a save) is no one's.
  for (const auto& [tag, path] : kinds)
    if (!slotted.contains(tag) && tag != mine &&
        std::find(live.begin(), live.end(), tag) == live.end()) {
      std::error_code rm;
      fs::remove(path, rm);
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

bool Autosaver::IsUnsavedSlotPath(const std::string& path) {
  for (fs::path p(path); !p.empty() && p != p.root_path(); p = p.parent_path())
    if (IsUnsavedSlot(p.filename().string()))
      return true;
  return false;
}

std::string Autosaver::SlotTag(const std::string& filename) {
  return IsUnsavedSlot(filename) ? TagOf(filename) : std::string();
}

namespace {

int64_t UnixTime(fs::file_time_type t) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::clock_cast<std::chrono::system_clock>(t)
                 .time_since_epoch())
      .count();
}

// Every unsaved slot in `stateDir`, one per tag: its file and its directory
// taken together, named by whichever was written last.
std::map<std::string, Autosaver::UnsavedSlot>
SlotsByTag(const std::string& stateDir) {
  std::map<std::string, Autosaver::UnsavedSlot> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(stateDir, ec)) {
    const std::string name = e.path().filename().string();
    if (!Autosaver::IsUnsavedSlot(name))
      continue;
    std::error_code tec;
    const int64_t when = UnixTime(fs::last_write_time(e.path(), tec));
    size_t documents = 0;
    if (e.is_directory(tec)) {
      // One file per document (`parts/Stand.json`); a temp file mid-write
      // is not one.
      for (const auto& f : fs::recursive_directory_iterator(e.path(), tec))
        if (f.is_regular_file(tec) && f.path().extension() == ".json")
          ++documents;
    } else if (e.is_regular_file(tec) && fs::file_size(e.path(), tec) > 0) {
      documents = 1;
    }
    Autosaver::UnsavedSlot& slot = out[TagOf(name)];
    if (slot.tag.empty() && !TagOf(name).empty())
      slot.kind = ReadKind(KindPath(stateDir, TagOf(name)));
    slot.tag = TagOf(name);
    slot.documents += documents;
    if (slot.path.empty() || when > slot.modifiedUnix ||
        (when == slot.modifiedUnix && !e.is_directory(tec))) {
      slot.path = e.path().string();
      slot.modifiedUnix = std::max(slot.modifiedUnix, when);
    }
  }
  return out;
}

} // namespace

std::vector<Autosaver::UnsavedSlot>
Autosaver::OrphanUnsavedSlots(const std::string& stateDir,
                              const std::vector<std::string>& live) {
  const std::string mine = InstanceTag();
  std::vector<UnsavedSlot> out;
  for (auto& [tag, slot] : SlotsByTag(stateDir)) {
    if (tag == mine || slot.documents == 0 ||
        std::find(live.begin(), live.end(), tag) != live.end())
      continue;
    out.push_back(std::move(slot));
  }
  std::sort(out.begin(), out.end(),
            [](const UnsavedSlot& a, const UnsavedSlot& b) {
              return a.modifiedUnix != b.modifiedUnix
                         ? a.modifiedUnix > b.modifiedUnix
                         : a.tag > b.tag;
            });
  return out;
}

std::optional<Autosaver::UnsavedSlot>
Autosaver::SlotOf(const std::string& stateDir, const std::string& tag) {
  auto slots = SlotsByTag(stateDir);
  const auto it = slots.find(tag);
  if (it == slots.end() || it->second.documents == 0)
    return std::nullopt;
  return it->second;
}

bool Autosaver::DiscardUnsavedSlot(const std::string& stateDir,
                                   const std::string& tag, std::string* error) {
  // Only an unsaved slot, only by its tag, only in the state dir: what this
  // deletes is decided here, never by a path someone handed in.
  if (tag == InstanceTag() || tag.find_first_of("/\\:") != std::string::npos ||
      tag == "." || tag == "..") {
    if (error)
      *error = tag == InstanceTag() ? "that is this instance's own unsaved work"
                                    : "'" + tag + "' is not an unsaved slot";
    return false;
  }
  // "" is the shared slot builds before the instance tag wrote.
  const std::string stem =
      tag.empty() ? std::string("unsaved") : kUnsavedPrefix + tag;
  std::error_code ec;
  bool any = false;
  for (const char* suffix : {kFileSuffix, kDirSuffix}) {
    const fs::path p = fs::path(stateDir) / (stem + suffix);
    if (fs::exists(p, ec)) {
      fs::remove_all(p, ec);
      if (ec) {
        if (error)
          *error = "could not delete " + p.string() + ": " + ec.message();
        return false;
      }
      any = true;
    }
  }
  if (any && !tag.empty())
    fs::remove(KindPath(stateDir, tag), ec);
  if (!any && error)
    *error = "no unsaved work named " + tag + " in " + stateDir;
  return any;
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
  if (projectPath.empty())
    fs::remove(KindPath(StateDir(), InstanceTag()), ec);
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

  if (projectPath.empty())
    NoteSlotKind(); // the unsaved slot says who wrote it
  m_lastSaveTime = now;
  m_lastSavedDocInstance = doc.InstanceId();
  m_lastSavedDocVersion = doc.SaveStamp();
  LOG_INFO("Autosaved to {}", target.string());
  return true;
}
