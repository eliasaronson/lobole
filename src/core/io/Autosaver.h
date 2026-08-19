#pragma once

#include <cstdint>
#include <string>

class Document;

// Timed autosave to a sidecar file with atomic replace (temp + rename), so a
// crash mid-write can never corrupt the previous autosave. GUI-free: pumped
// from both the GUI frame loop and the headless loop. Design:
// docs/versioning/f1-f2.md (F2).
class Autosaver {
public:
  explicit Autosaver(double intervalSeconds = 60.0)
      : m_interval(intervalSeconds) {}

  void SetInterval(double seconds) { m_interval = seconds; }
  double Interval() const { return m_interval; }

  // Anchor the interval at loop start so the first autosave happens one full
  // interval in, not on the first dirty edit.
  void Prime(double now) { m_lastSaveTime = now; }

  // The sidecar for a project path; "" = the unsaved-document slot under the
  // XDG state dir (~/.local/state/designer/unsaved.autosave.json).
  static std::string SidecarPath(const std::string& projectPath);

  // The app's XDG state dir (~/.local/state/designer) — home of the unsaved
  // autosave slot and the import staging area (sidecars extracted before the
  // project has a folder to own them).
  static std::string StateDir();

  // A sidecar exists and is newer than its project file (always true for an
  // existing sidecar whose project file is missing or path is empty).
  static bool HasNewerAutosave(const std::string& projectPath);

  // Remove the sidecar (clean save; clean exit when not dirty).
  static void Clear(const std::string& projectPath);

  // Call regularly with a monotonic clock. Saves when enabled (interval > 0),
  // dirty (doc version != savedVersion), changed since the last autosave, and
  // the interval elapsed. Returns true when a save happened.
  bool MaybeSave(const Document& doc, const std::string& projectPath,
                 uint64_t savedVersion, double now,
                 std::string* error = nullptr);

private:
  double m_interval;
  double m_lastSaveTime = 0.0;
  // WHICH document was at that version, not just the number: versions restart
  // at 0 on every document replacement, so a version alone is not an identity.
  uint64_t m_lastSavedDocInstance = 0;
  uint64_t m_lastSavedDocVersion = 0;
};
