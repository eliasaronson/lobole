#include "core/io/Autosaver.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string Autosaver::StateDir() {
  return RuntimePaths::UserStateDir().string();
}

std::string Autosaver::SidecarPath(const std::string& projectPath) {
  if (projectPath.empty())
    return (fs::path(StateDir()) / "unsaved.autosave.json").string();
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
    return (fs::path(StateDir()) / "unsaved.autosave.d").string();
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
  if (doc.Version() == savedVersion)
    return false; // not dirty
  if (doc.InstanceId() == m_lastSavedDocInstance &&
      doc.Version() == m_lastSavedDocVersion)
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
  m_lastSavedDocVersion = doc.Version();
  LOG_INFO("Autosaved to {}", target.string());
  return true;
}
