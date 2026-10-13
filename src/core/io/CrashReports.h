// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Crash reports a user can hand over (docs/architecture.md#crashes-and-stalls).
//
// Every run holds a SESSION MARKER, `<state>/sessions/<started>-<pid>.json`,
// with an exclusive lock on it for its whole life (platform::FileLock). A
// clean exit deletes it. A marker whose lock can be taken therefore belongs
// to a process that is gone — crashed, OOM-killed, `kill -9`, power lost —
// and the next start turns it into a folder under `<state>/crashes/`: what
// happened, the session's log tail, the crash handler's stack and dump, and
// copies of that session's autosaves. Nothing is ever sent anywhere.
//
// In core rather than the app because the RPC, the GUI's notice and the unit
// tests all read it; only the signal handler that writes the `.crash` file
// lives in the executable (app/CrashHandler.cpp).
namespace crash {

inline constexpr size_t kKeepReports = 20;
inline constexpr size_t kKeepLogs = 10;
inline constexpr size_t kLogTailLines = 2000;

// This process's session: the names its files go by.
struct Session {
  std::string stem; // "<YYYYmmdd-HHMMSS>-<pid>"
  std::filesystem::path stateDir;
  std::filesystem::path marker;    // sessions/<stem>.json, locked
  std::filesystem::path crashFile; // sessions/<stem>.crash, by the handler
  std::filesystem::path dumpFile;  // sessions/<stem>.dmp, Windows only
  std::filesystem::path logFile;   // logs/<stem>.log
};

// Starts this process's session under `stateDir`: opens the session log
// (logfile::Open), creates the marker — version, build, mode, OS, log path,
// project — and holds its lock until EndSession. `mode` is "windowed",
// "offscreen", "headless" or "gui-tests". Nullopt when the marker cannot be
// made, and the app runs on without crash reports (said in the log).
std::optional<Session> BeginSession(const std::filesystem::path& stateDir,
                                    std::string_view mode);
// The session BeginSession started, or null.
const Session* CurrentSession();
// The open project, as the marker should name it. Rewrites the marker only
// when it changed, so every main-loop iteration may call it.
void NoteProject(const std::string& projectPath);
// Clean exit: delete the marker and release its lock.
void EndSession();

// One report folder.
struct Report {
  std::filesystem::path dir; // crashes/<stem>
  std::string stem;
  // "crashed: SIGSEGV", "crashed: Windows exception 0xC0000005 ...", or
  // "ended without a crash report: killed, out of memory, or power lost".
  std::string reason;
  bool crashed = false; // the crash handler wrote a file
  std::string started;  // local time, "2026-10-12 14:30:05"
  std::string mode, version, build, os;
  std::string project; // "" = a project never saved
  // The copies under dir/autosave, relative to it: "Part.dsgn.autosave",
  // "Part.dsgn.autosave.d/parts/Stand.json", "unsaved.autosave.json".
  std::vector<std::string> autosaves;
};

// Every unlocked marker in `<stateDir>/sessions` becomes a report folder,
// then the marker and its crash files are deleted; reports beyond `keep`
// and logs beyond kKeepLogs (never a live session's) are removed. Returns
// the new reports, newest first. A marker some live process holds is left
// alone — including this process's own.
std::vector<Report> CollectDeadSessions(const std::filesystem::path& stateDir,
                                        size_t keep = kKeepReports);

// The reports on disk, newest first.
std::vector<Report> ListReports(const std::filesystem::path& stateDir);
std::filesystem::path ReportsDir(const std::filesystem::path& stateDir);

// The GUI's notice: the report start-up produced, shown once by the modal
// ("Loböle closed unexpectedly last time").
void SetNotice(std::optional<Report> report);
const std::optional<Report>& Notice();

} // namespace crash
