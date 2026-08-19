#include "core/io/Autosaver.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/io/Serialization.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string Autosaver::StateDir() {
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
    return (fs::path(xdg) / "designer").string();
  if (const char* home = std::getenv("HOME"); home && *home)
    return (fs::path(home) / ".local" / "state" / "designer").string();
  return (fs::temp_directory_path() / "designer-state").string();
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
