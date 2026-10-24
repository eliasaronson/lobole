// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Document;

// Timed autosave to a sidecar file with atomic replace (temp + rename), so a
// crash mid-write can never corrupt the previous autosave. GUI-free: pumped
// from both the GUI frame loop and the headless loop. Design:
// docs/versioning/README.md#autosave-and-recovery.
class Autosaver {
public:
  explicit Autosaver(double intervalSeconds = 60.0)
      : m_interval(intervalSeconds) {}

  void SetInterval(double seconds) { m_interval = seconds; }
  double Interval() const { return m_interval; }

  // Anchor the interval at loop start so the first autosave happens one full
  // interval in, not on the first dirty edit.
  void Prime(double now) { m_lastSaveTime = now; }

  // The sidecar for a project path; "" = THIS instance's unsaved-document
  // slot under the XDG state dir
  // (~/.local/state/lobole/unsaved-<InstanceTag()>.autosave.json). Used for
  // a project that IS one document; see SidecarDir for the rest.
  static std::string SidecarPath(const std::string& projectPath);

  // What names this process's unsaved slot. Every instance on a machine
  // shares the state dir -- the human's app and each agent's
  // headless-preview instance -- and the slot used to be ONE file there, so
  // one instance's autosave wrote over another's recovery copy and its save
  // (which clears "the unsaved slot") deleted it.
  //
  // crash::BeginSession sets its session stem ("20261024-031500-4242"),
  // before the session marker lists the slot: a crash report then copies
  // the dead session's own slot, and CollectDeadSessions knows which slots
  // belong to a live session. Until then, and in a process without a
  // session, "pid<pid>". A tag that could leave the state dir is refused.
  static void SetInstanceTag(const std::string& tag);
  static std::string InstanceTag();

  // Is this file name an unsaved slot -- any instance's, or the shared
  // `unsaved.autosave.json` / `.d` builds before the tag wrote?
  [[nodiscard]] static bool IsUnsavedSlot(const std::string& filename);

  // Deletes the unsaved slots in `stateDir` that no live instance owns --
  // tags not in `live` and not this process's -- beyond the newest `keep`.
  // A dirty exit keeps its slot, and nothing offers it again, so without
  // this every never-saved session would leave one behind for good. Called
  // by crash::CollectDeadSessions AFTER it has copied the dead sessions'
  // slots into their reports.
  static void PruneUnsavedSlots(const std::string& stateDir,
                                const std::vector<std::string>& live,
                                size_t keep);

  // Where a multi-document project's sidecars live: one file per DIRTY
  // document, mirroring the layout project.json describes
  // (`<project>.autosave/parts/Stand.json`).
  //
  // A directory rather than one file, and one file per document rather than
  // one for the project, because F2 chose atomic temp+rename for a reason
  // that still holds: a tool whose whole job is surviving a crash must never
  // hand a half-written recovery file to someone who has already lost work
  // once. Writing a project tree into a sidecar would throw that away; a file
  // each keeps every one complete-or-absent, and stops rewriting 48
  // unchanged parts every minute.
  static std::string SidecarDir(const std::string& projectPath);

  // The documents with a sidecar newer than their saved form, as the `dir`
  // values project.json uses ("parts/Stand"). Empty when there is nothing to
  // recover.
  static std::vector<std::string>
  RecoverableDocuments(const std::string& projectPath);

  // The app's XDG state dir (~/.local/state/lobole) — home of the unsaved
  // autosave slot and the import staging area (sidecars extracted before the
  // project has a folder to own them).
  static std::string StateDir();

  // A sidecar exists and is newer than its project file (always true for an
  // existing sidecar whose project file is missing or path is empty).
  static bool HasNewerAutosave(const std::string& projectPath);

  // Remove the sidecar (clean save; clean exit when not dirty). "" is this
  // instance's unsaved slot, never another instance's.
  static void Clear(const std::string& projectPath);

  // Call regularly with a monotonic clock. Saves when enabled (interval > 0),
  // dirty (doc.SaveStamp() != savedVersion — the stamp, not Version(), so an
  // edit that regenerates nothing still counts), changed since the last
  // autosave, and the interval elapsed. Returns true when a save happened.
  // NeedsSave below takes the same stamps.
  //
  // For a project that is ONE document. A project with several is autosaved
  // by ui::AutosaveProject, which owns the iteration because only the UI
  // layer knows what documents a project has — this used to be handed
  // `state.document`, the ACTIVE TAB, so a multi-document project autosaved
  // one part and an assembly tab autosaved nothing at all (F10).
  bool MaybeSave(const Document& doc, const std::string& projectPath,
                 uint64_t savedVersion, double now,
                 std::string* error = nullptr);

  // --- the pieces ui::AutosaveProject drives, for several documents -------

  // Has the interval elapsed? (Whether anything is dirty is the caller's
  // question — it knows what documents there are.)
  [[nodiscard]] bool Due(double now) const {
    return m_interval > 0.0 && now - m_lastSaveTime >= m_interval;
  }
  void MarkSaved(double now) { m_lastSaveTime = now; }

  // Is this document worth writing? `instanceId` distinguishes WHICH document
  // has been at that version — versions restart at 0 on every document
  // replacement, so a version alone is not an identity. Assemblies and
  // drawings have no instance id; they pass 0 and rely on Forget() at project
  // open, which is where a replacement realistically happens for them.
  [[nodiscard]] bool NeedsSave(uint64_t docId, uint64_t instanceId,
                               uint64_t version, uint64_t savedVersion) const;
  void NoteSaved(uint64_t docId, uint64_t instanceId, uint64_t version);

  // A different project (or the same one reopened): nothing tracked applies.
  void Forget() { m_saved.clear(); }

  // Call before autosaving: forgets the per-document tracking when the
  // project has been replaced under us.
  void NoteProject(const std::string& path, uint64_t generation) {
    if (path == m_path && generation == m_generation)
      return;
    m_path = path;
    m_generation = generation;
    Forget();
  }

private:
  double m_interval;
  double m_lastSaveTime = 0.0;
  // WHICH document was at that version, not just the number: versions restart
  // at 0 on every document replacement, so a version alone is not an identity.
  uint64_t m_lastSavedDocInstance = 0;
  uint64_t m_lastSavedDocVersion = 0;
  // The same question per document, keyed by the tab's stable id.
  struct Seen {
    uint64_t instanceId = 0;
    uint64_t version = 0;
  };
  std::map<uint64_t, Seen> m_saved;
  std::string m_path;
  uint64_t m_generation = 0;
};
