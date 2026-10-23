// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/CrashReports.h"

#include "common/BuildInfo.h"
#include "common/LogFile.h"
#include "common/Logger.h"
#include "common/Platform.h"
#include "core/io/Autosaver.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>

namespace fs = std::filesystem;

namespace crash {

namespace {

std::mutex g_mutex;
std::optional<Session> g_session;
platform::FileLock g_lock;
nlohmann::json g_marker;
std::string g_project;
bool g_projectNoted = false;
std::optional<Report> g_notice;
std::optional<std::string> g_startupFailureNotice;

std::tm LocalTime(std::time_t t) {
  std::tm out{};
#if defined(_WIN32)
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
  return out;
}

std::string Format(std::time_t t, const char* pattern) {
  const std::tm tm = LocalTime(t);
  char buf[64];
  const size_t n = std::strftime(buf, sizeof buf, pattern, &tm);
  return std::string(buf, n);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool WriteFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  return static_cast<bool>(out);
}

// The last `lines` lines of a file, read from the end: a long session's log
// can be large, and only its tail is wanted.
std::string Tail(const fs::path& path, size_t lines) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  std::string out;
  constexpr std::streamoff kChunk = 64 * 1024;
  std::streamoff at = size;
  size_t newlines = 0;
  while (at > 0) {
    const std::streamoff step = std::min(kChunk, at);
    at -= step;
    std::string chunk(static_cast<size_t>(step), '\0');
    in.seekg(at);
    in.read(chunk.data(), step);
    out.insert(0, chunk);
    newlines = static_cast<size_t>(std::count(out.begin(), out.end(), '\n'));
    // One more than wanted: the last line ends in a newline of its own.
    if (newlines > lines)
      break;
  }
  if (newlines > lines) {
    size_t skip = newlines - lines;
    size_t pos = 0;
    while (skip > 0 && pos < out.size()) {
      if (out[pos] == '\n')
        --skip;
      ++pos;
    }
    out.erase(0, pos);
  }
  return out;
}

std::time_t ToTimeT(fs::file_time_type t) {
  return std::chrono::system_clock::to_time_t(
      std::chrono::clock_cast<std::chrono::system_clock>(t));
}

// The autosave files a session's project writes: the single-document sidecar
// and the per-document folder (core/io/Autosaver.h).
std::vector<std::string> AutosavesOf(const std::string& projectPath) {
  return {Autosaver::SidecarPath(projectPath),
          Autosaver::SidecarDir(projectPath)};
}

bool WriteMarker() { return g_lock.Replace(g_marker.dump(2) + "\n"); }

// Copies what the session's autosave paths hold that it wrote — no older
// than the session itself, since a clean quit with unsaved changes leaves an
// earlier session's sidecar in the same place. Returns the copies, relative
// to `into`.
std::vector<std::string> CopyAutosaves(const nlohmann::json& marker,
                                       std::time_t started,
                                       const fs::path& into) {
  std::vector<std::string> copied;
  if (!marker.contains("autosaves") || !marker["autosaves"].is_array())
    return copied;
  std::error_code ec;
  // To the second, with no slack: the unsaved slot is shared, and the
  // previous session's last autosave can be a second or two older than the
  // next one's start (seen under Wine, where a crash and a relaunch take
  // that long). Both times are on this machine's clock.
  const auto fresh = [&](const fs::path& p) {
    const auto t = fs::last_write_time(p, ec);
    return !ec && ToTimeT(t) >= started;
  };
  for (const auto& entry : marker["autosaves"]) {
    if (!entry.is_string())
      continue;
    const fs::path source = entry.get<std::string>();
    if (fs::is_regular_file(source, ec)) {
      if (!fresh(source))
        continue;
      fs::create_directories(into, ec);
      if (fs::copy_file(source, into / source.filename(),
                        fs::copy_options::overwrite_existing, ec))
        copied.push_back(source.filename().generic_string());
    } else if (fs::is_directory(source, ec)) {
      for (auto it = fs::recursive_directory_iterator(source, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !fresh(it->path()))
          continue;
        const fs::path rel =
            source.filename() / fs::relative(it->path(), source, ec);
        fs::create_directories((into / rel).parent_path(), ec);
        if (fs::copy_file(it->path(), into / rel,
                          fs::copy_options::overwrite_existing, ec))
          copied.push_back(rel.generic_string());
      }
    }
  }
  return copied;
}

std::string ReasonFrom(const std::string& crashText, bool* crashed) {
  // The handler's first line: "reason: SIGSEGV".
  const std::string key = "reason: ";
  if (crashText.rfind(key, 0) == 0) {
    *crashed = true;
    return "crashed: " +
           crashText.substr(key.size(), crashText.find('\n') - key.size());
  }
  if (!crashText.empty()) {
    *crashed = true;
    return "crashed (the crash report is unreadable)";
  }
  *crashed = false;
#if defined(_WIN32)
  // A stop from the console deletes the marker (app/CrashHandler.cpp), and
  // running out of memory is a failed allocation, which the crash handler
  // sees. What it cannot see is TerminateProcess.
  return "ended without a crash report: ended from outside (Task Manager, "
         "taskkill /F) or power lost";
#else
  return "ended without a crash report: killed, out of memory, or power lost";
#endif
}

std::string ReportText(const Report& r, const std::string& crashText,
                       bool hasDump, const std::string& collected,
                       const std::string& explanation = "") {
  std::ostringstream o;
  o << "Loböle crash report\n\n"
       "This folder may hold your design (the copies in autosave/) and the\n"
       "names and paths of your files (session.log). Nothing has been sent\n"
       "anywhere: share it only with someone you would show the design to.\n\n";
  const auto row = [&](const char* label, const std::string& value) {
    o << label << std::string(15 - std::string(label).size(), ' ') << value
      << "\n";
  };
  row("What happened:", r.reason);
  row("Started:", r.started.empty() ? "unknown" : r.started);
  row("Collected:", collected);
  row("Version:", r.version.empty() ? "unknown" : r.version);
  row("Build:", r.build.empty() ? "unknown" : r.build);
  row("OS:", r.os.empty() ? "unknown" : r.os);
  row("Mode:", r.mode.empty() ? "unknown" : r.mode);
  row("Project:", r.project.empty() ? "(never saved)" : r.project);
  row("Log:",
      "session.log (the last " + std::to_string(kLogTailLines) + " lines)");
  row("Autosaves:", r.autosaves.empty()
                        ? std::string("none")
                        : "autosave/ (" + std::to_string(r.autosaves.size()) +
                              " file" + (r.autosaves.size() == 1 ? "" : "s") +
                              ")");
  if (hasDump)
    row("Dump:", "crash.dmp");
  o << "\n";
  if (!explanation.empty()) {
    o << explanation;
  } else if (crashText.empty()) {
#if defined(_WIN32)
    o << "There is no stack: the process ended without running its crash\n"
         "handler, so it was ended from outside (Task Manager's End task,\n"
         "taskkill /F) or the machine lost power. The tail of session.log is\n"
         "what it was doing.\n";
#else
    o << "There is no stack: the process ended without running its crash\n"
         "handler, so it was killed (kill -9, the out-of-memory killer) or\n"
         "the machine lost power. The tail of session.log is what it was\n"
         "doing.\n";
#endif
  } else {
    o << "--- the crash handler's report (raw frames are module+offset;\n"
         "--- tools/symbolize-crash.py report.txt BINARY names them) ---\n"
      << crashText;
    if (!crashText.empty() && crashText.back() != '\n')
      o << "\n";
  }
  return o.str();
}

Report FromJson(const nlohmann::json& j, const fs::path& dir) {
  Report r;
  r.dir = dir;
  r.stem = j.value("stem", dir.filename().string());
  r.reason = j.value("reason", "");
  r.crashed = j.value("crashed", false);
  r.started = j.value("started", "");
  r.mode = j.value("mode", "");
  r.version = j.value("version", "");
  r.build = j.value("build", "");
  r.os = j.value("os", "");
  r.project = j.value("project", "");
  r.startedEpoch = j.value("started_epoch", std::time_t{0});
  r.startedMs = j.value("started_ms", int64_t{r.startedEpoch} * 1000);
  r.firstFrame = j.value("first_frame", false);
  if (j.contains("autosaves") && j["autosaves"].is_array())
    for (const auto& a : j["autosaves"])
      if (a.is_string())
        r.autosaves.push_back(a.get<std::string>());
  return r;
}

nlohmann::json ToJson(const Report& r) {
  return {{"stem", r.stem},
          {"reason", r.reason},
          {"crashed", r.crashed},
          {"started", r.started},
          {"mode", r.mode},
          {"version", r.version},
          {"build", r.build},
          {"os", r.os},
          {"project", r.project},
          {"autosaves", r.autosaves},
          {"started_epoch", r.startedEpoch},
          {"started_ms", r.startedMs},
          {"first_frame", r.firstFrame}};
}

// One dead session into its report folder. `marker` is locked by us.
std::optional<Report> Collect(const fs::path& stateDir, const fs::path& marker,
                              const platform::FileLock& lock) {
  std::error_code ec;
  const std::string stem = marker.stem().string();
  const fs::path sessions = marker.parent_path();
  const fs::path crashFile = sessions / (stem + ".crash");
  const fs::path dumpFile = sessions / (stem + ".dmp");

  nlohmann::json j = nlohmann::json::object();
  if (auto text = lock.ReadAll()) {
    try {
      j = nlohmann::json::parse(*text);
    } catch (const std::exception&) {
      // Torn (it was being rewritten when the process died): report what
      // the crash file and the name still say.
      j = nlohmann::json::object();
    }
  }
  if (!j.is_object())
    j = nlohmann::json::object();

  fs::path dir = ReportsDir(stateDir) / stem;
  for (int n = 2; fs::exists(dir, ec); ++n)
    dir = ReportsDir(stateDir) / (stem + "-" + std::to_string(n));
  fs::create_directories(dir, ec);
  if (ec)
    return std::nullopt;

  Report r;
  r.dir = dir;
  r.stem = stem;
  const std::string crashText =
      fs::exists(crashFile, ec) ? ReadFile(crashFile) : std::string();
  r.reason = ReasonFrom(crashText, &r.crashed);
  r.started = j.value("started", "");
  r.mode = j.value("mode", "");
  r.version = j.value("version", "");
  r.build = j.value("build", "");
  r.os = j.value("os", "");
  r.project = j.value("project", "");
  const std::time_t startedAt = j.value("started_epoch", std::time_t{0});
  r.startedEpoch = startedAt;
  r.startedMs = j.value("started_ms", int64_t{startedAt} * 1000);
  r.firstFrame = j.value("first_frame", false);

  const std::string log = j.value("log", "");
  if (!log.empty())
    WriteFile(dir / "session.log", Tail(log, kLogTailLines));
  const bool hasDump = fs::exists(dumpFile, ec);
  if (hasDump)
    fs::copy_file(dumpFile, dir / "crash.dmp",
                  fs::copy_options::overwrite_existing, ec);
  r.autosaves = CopyAutosaves(j, startedAt, dir / "autosave");

  const std::string collected = Format(std::time(nullptr), "%Y-%m-%d %H:%M:%S");
  WriteFile(dir / "report.txt", ReportText(r, crashText, hasDump, collected));
  WriteFile(dir / "report.json", ToJson(r).dump(2) + "\n");

  fs::remove(crashFile, ec);
  fs::remove(dumpFile, ec);
  return r;
}

void PruneReports(const fs::path& stateDir, size_t keep) {
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(ReportsDir(stateDir), ec))
    if (e.is_directory(ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename() > b.filename();
  });
  for (size_t i = keep; i < dirs.size(); ++i)
    fs::remove_all(dirs[i], ec);
}

} // namespace

fs::path ReportsDir(const fs::path& stateDir) { return stateDir / "crashes"; }

std::optional<Session> BeginSession(const fs::path& stateDir,
                                    std::string_view mode) {
  std::lock_guard guard(g_mutex);
  if (g_session)
    return g_session;
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  Session s;
  s.stem =
      Format(t, "%Y%m%d-%H%M%S") + "-" + std::to_string(platform::CurrentPid());
  s.stateDir = stateDir;
  const fs::path sessions = stateDir / "sessions";
  s.marker = sessions / (s.stem + ".json");
  s.crashFile = sessions / (s.stem + ".crash");
  s.dumpFile = sessions / (s.stem + ".dmp");
  s.logFile = stateDir / "logs" / (s.stem + ".log");

  std::string error;
  if (!logfile::Open(s.logFile, &error))
    LOG_WARN("No session log file ({}): {}", s.logFile.string(), error);

  std::error_code ec;
  fs::create_directories(sessions, ec);
  // Locked BEFORE it has its name: a marker that exists unlocked for an
  // instant could be collected as dead by another instance starting at the
  // same moment. The lock belongs to the file, so the rename keeps it.
  const fs::path pending = sessions / (s.stem + ".json.new");
  if (!g_lock.TryLock(pending, /*create=*/true)) {
    LOG_WARN("No crash reports this session: cannot lock {}", pending.string());
    return std::nullopt;
  }
  g_marker = {
      {"stem", s.stem},
      {"version", BuildInfo::Version()},
      {"build", BuildInfo::Commit() + ", " + BuildInfo::Configuration()},
      {"mode", std::string(mode)},
      {"os", platform::OsDescription()},
      {"pid", platform::CurrentPid()},
      {"started", Format(t, "%Y-%m-%d %H:%M:%S")},
      {"started_epoch", t},
      {"started_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count()},
      {"log", s.logFile.string()},
      {"project", ""},
      {"autosaves", AutosavesOf("")}};
  if (!WriteMarker()) {
    g_lock.Release();
    fs::remove(pending, ec);
    LOG_WARN("No crash reports this session: cannot write {}",
             pending.string());
    return std::nullopt;
  }
  fs::rename(pending, s.marker, ec);
  if (ec) {
    g_lock.Release();
    fs::remove(pending, ec);
    LOG_WARN("No crash reports this session: cannot name {}",
             s.marker.string());
    return std::nullopt;
  }
  g_session = s;
  return g_session;
}

const Session* CurrentSession() {
  std::lock_guard guard(g_mutex);
  return g_session ? &*g_session : nullptr;
}

void NoteProject(const std::string& projectPath) {
  std::lock_guard guard(g_mutex);
  if (!g_session || (g_projectNoted && projectPath == g_project))
    return;
  g_project = projectPath;
  g_projectNoted = true;
  g_marker["project"] = projectPath;
  g_marker["autosaves"] = AutosavesOf(projectPath);
  if (!WriteMarker())
    LOG_WARN("Could not update the session marker {}",
             g_session->marker.string());
}

void EndSession() {
  std::lock_guard guard(g_mutex);
  if (!g_session)
    return;
  std::error_code ec;
  // Deleted while still locked, so no collector can take it in between.
  fs::remove(g_session->marker, ec);
  fs::remove(g_session->crashFile, ec);
  fs::remove(g_session->dumpFile, ec);
  g_lock.Release();
  g_session.reset();
}

void NoteFirstFrame() {
  std::lock_guard guard(g_mutex);
  if (!g_session || g_marker.value("first_frame", false))
    return;
  g_marker["first_frame"] = true;
  if (!WriteMarker())
    LOG_WARN("Could not update the session marker {}",
             g_session->marker.string());
  WriteFile(LastGoodStartFile(g_session->stateDir),
            std::to_string(g_marker.value("started_ms", int64_t{0})) + "\n");
}

fs::path LastGoodStartFile(const fs::path& stateDir) {
  return stateDir / "last-good-start";
}

std::optional<Report> ReportThisSession(const std::string& reason,
                                        bool recovered) {
  std::lock_guard guard(g_mutex);
  if (!g_session)
    return std::nullopt;
  std::error_code ec;
  const fs::path& stateDir = g_session->stateDir;
  fs::path dir = ReportsDir(stateDir) / g_session->stem;
  for (int n = 2; fs::exists(dir, ec); ++n)
    dir = ReportsDir(stateDir) / (g_session->stem + "-" + std::to_string(n));
  fs::create_directories(dir, ec);
  if (ec)
    return std::nullopt;
  Report r;
  r.dir = dir;
  r.stem = dir.filename().string();
  r.reason = reason;
  r.started = g_marker.value("started", "");
  r.mode = g_marker.value("mode", "");
  r.version = g_marker.value("version", "");
  r.build = g_marker.value("build", "");
  r.os = g_marker.value("os", "");
  r.project = g_marker.value("project", "");
  r.startedEpoch = g_marker.value("started_epoch", std::time_t{0});
  r.startedMs = g_marker.value("started_ms", int64_t{r.startedEpoch} * 1000);
  r.firstFrame = g_marker.value("first_frame", false);
  const std::string log = g_marker.value("log", "");
  if (!log.empty())
    WriteFile(dir / "session.log", Tail(log, kLogTailLines));
  r.autosaves = CopyAutosaves(g_marker, r.startedEpoch, dir / "autosave");
  const std::string collected = Format(std::time(nullptr), "%Y-%m-%d %H:%M:%S");
  WriteFile(dir / "report.txt",
            ReportText(r, "", false, collected,
                       recovered
                           ? "There is no stack: the process did not crash. "
                             "It\nsaved what was unsaved, rebuilt its "
                             "graphics device in\nplace and carried on; "
                             "this report is the evidence.\nThe tail of "
                             "session.log is what it was doing.\n"
                           : "There is no stack: the process did not crash. "
                             "It\nwrote this report when it happened, saved "
                             "what was\nunsaved, and asked to restart. The "
                             "tail of\nsession.log is what it was doing.\n"));
  WriteFile(dir / "report.json", ToJson(r).dump(2) + "\n");
  PruneReports(stateDir, kKeepReports);
  LOG_WARN("This session's report ({}) is in {}", reason, dir.string());
  return r;
}

size_t EarlyDeathsInARow(const std::vector<Report>& reports,
                         int64_t lastGoodStartMs) {
  size_t count = 0;
  for (const Report& r : reports) {
    if (r.mode != "windowed")
      continue;
    if (r.firstFrame || (lastGoodStartMs > 0 && r.startedMs < lastGoodStartMs))
      break;
    // A device loss is reported by a session that drew; a report of ours
    // that is not a death does not count either way.
    if (r.reason.rfind(kDeviceLostReason, 0) == 0 ||
        r.reason.rfind(kDeviceRecoveredReason, 0) == 0)
      break;
    ++count;
  }
  return count;
}

size_t EarlyDeathsInARow(const fs::path& stateDir) {
  int64_t lastGood = 0;
  std::error_code ec;
  if (fs::exists(LastGoodStartFile(stateDir), ec)) {
    try {
      lastGood = std::stoll(ReadFile(LastGoodStartFile(stateDir)));
    } catch (const std::exception&) {
      lastGood = 0; // unreadable: as if there were none
    }
  }
  return EarlyDeathsInARow(ListReports(stateDir), lastGood);
}

std::vector<Report> CollectDeadSessions(const fs::path& stateDir, size_t keep) {
  std::vector<Report> out;
  std::error_code ec;
  const fs::path sessions = stateDir / "sessions";
  std::vector<fs::path> markers;
  for (const auto& e : fs::directory_iterator(sessions, ec)) {
    if (e.path().extension() == ".json") {
      markers.push_back(e.path());
    } else if (e.path().extension() == ".new") {
      // A marker that never got its name: its process died between
      // creating it and the rename. Nothing to report; just not left about.
      platform::FileLock pending;
      if (pending.TryLock(e.path(), /*create=*/false)) {
        std::error_code rm;
        fs::remove(e.path(), rm);
      }
    }
  }
  std::vector<fs::path> liveLogs;
  for (const fs::path& marker : markers) {
    platform::FileLock lock;
    if (!lock.TryLock(marker, /*create=*/false)) {
      // Somebody holds it: a live session (perhaps this one). Its log stays.
      liveLogs.push_back(stateDir / "logs" / (marker.stem().string() + ".log"));
      continue;
    }
    if (auto r = Collect(stateDir, marker, lock)) {
      LOG_WARN("The session {} ended badly ({}); its report is in {}", r->stem,
               r->reason, r->dir.string());
      out.push_back(std::move(*r));
    }
    // Deleted while still locked: a second collector starting at the same
    // moment either fails the lock or finds the file gone.
    fs::remove(marker, ec);
    lock.Release();
  }
  std::sort(out.begin(), out.end(),
            [](const Report& a, const Report& b) { return a.stem > b.stem; });
  PruneReports(stateDir, keep);
  logfile::Prune(stateDir / "logs", kKeepLogs, liveLogs);
  return out;
}

std::vector<Report> ListReports(const fs::path& stateDir) {
  std::vector<Report> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(ReportsDir(stateDir), ec)) {
    if (!e.is_directory(ec))
      continue;
    nlohmann::json j = nlohmann::json::object();
    try {
      j = nlohmann::json::parse(ReadFile(e.path() / "report.json"));
    } catch (const std::exception&) {
      j = {{"reason", "unreadable report"}};
    }
    out.push_back(FromJson(j, e.path()));
  }
  std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) {
    return a.dir.filename() > b.dir.filename();
  });
  return out;
}

void SetNotice(std::optional<Report> report) {
  std::lock_guard guard(g_mutex);
  g_notice = std::move(report);
}

const std::optional<Report>& Notice() {
  // Read on the main thread only (the GUI's modal), like SetNotice.
  return g_notice;
}

void SetStartupFailureNotice(std::optional<std::string> text) {
  std::lock_guard guard(g_mutex);
  g_startupFailureNotice = std::move(text);
}

const std::optional<std::string>& StartupFailureNotice() {
  return g_startupFailureNotice; // main thread only, as Notice
}

} // namespace crash
