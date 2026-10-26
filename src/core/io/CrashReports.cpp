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

#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>

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
// A field of a session marker or a report, or `def` when it is absent or
// not the type it is written as. One field at a time, as the settings file
// reads its rows: a damaged report.json, or a marker torn by the death it
// records, loses that field -- `j.value()` threw out of the start instead,
// before the window opened.
template <class T> T Get(const nlohmann::json& j, const char* key, T def) {
  if (!j.is_object())
    return def;
  const auto it = j.find(key);
  if (it == j.end())
    return def;
  if constexpr (std::is_same_v<T, bool>) {
    return it->is_boolean() ? it->get<bool>() : def;
  } else if constexpr (std::is_same_v<T, std::string>) {
    return it->is_string() ? it->get<std::string>() : def;
  } else {
    if (!it->is_number_integer())
      return def;
    if (it->is_number_unsigned() &&
        it->get<uint64_t>() >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return def;
    return static_cast<T>(it->get<int64_t>());
  }
}

// The start in milliseconds, from the marker's own or from its seconds --
// never by a multiplication that overflows on a damaged one.
int64_t StartedMs(const nlohmann::json& j, std::time_t epoch) {
  const int64_t ms = Get<int64_t>(j, "started_ms", 0);
  if (ms != 0 || !j.contains("started_epoch"))
    return ms;
  const int64_t limit = std::numeric_limits<int64_t>::max() / 1000;
  return epoch > limit || epoch < -limit ? 0 : int64_t{epoch} * 1000;
}

// What a marker may name as an autosave: a sidecar or a sidecar directory
// (Autosaver), never anything else. A damaged marker naming "/" would have
// copied every file newer than its start into the report.
bool IsAutosavePath(const fs::path& p) {
  const std::string name = p.filename().string();
  return name.ends_with(".autosave") || name.ends_with(".autosave.d") ||
         Autosaver::IsUnsavedSlot(name);
}

std::vector<std::string> CopyAutosaves(const nlohmann::json& marker,
                                       std::time_t started,
                                       const fs::path& into) {
  std::vector<std::string> copied;
  if (!marker.contains("autosaves") || !marker["autosaves"].is_array())
    return copied;
  std::error_code ec;
  // To the second, with no slack: a project's sidecar outlives the session
  // that wrote it, and the previous session's last autosave can be a second
  // or two older than the next one's start (seen under Wine, where a crash
  // and a relaunch take that long). Both times are on this machine's clock.
  // (The unsaved slot is the session's own, named by its stem: Autosaver.h.)
  const auto fresh = [&](const fs::path& p) {
    const auto t = fs::last_write_time(p, ec);
    return !ec && ToTimeT(t) >= started;
  };
  // Each once: a damaged marker listing one sidecar a million times copied
  // it a million times.
  std::set<std::string> seen;
  for (const auto& entry : marker["autosaves"]) {
    if (!entry.is_string() || !seen.insert(entry.get<std::string>()).second)
      continue;
    const fs::path source = entry.get<std::string>();
    if (!IsAutosavePath(source))
      continue;
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
  r.stem = Get<std::string>(j, "stem", dir.filename().string());
  r.reason = Get<std::string>(j, "reason", "");
  r.crashed = Get<bool>(j, "crashed", false);
  r.started = Get<std::string>(j, "started", "");
  r.mode = Get<std::string>(j, "mode", "");
  r.version = Get<std::string>(j, "version", "");
  r.build = Get<std::string>(j, "build", "");
  r.os = Get<std::string>(j, "os", "");
  r.project = Get<std::string>(j, "project", "");
  r.startedEpoch = Get<std::time_t>(j, "started_epoch", 0);
  r.startedMs = StartedMs(j, r.startedEpoch);
  r.firstFrame = Get<bool>(j, "first_frame", false);
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
  r.started = Get<std::string>(j, "started", "");
  r.mode = Get<std::string>(j, "mode", "");
  r.version = Get<std::string>(j, "version", "");
  r.build = Get<std::string>(j, "build", "");
  r.os = Get<std::string>(j, "os", "");
  r.project = Get<std::string>(j, "project", "");
  const std::time_t startedAt = Get<std::time_t>(j, "started_epoch", 0);
  r.startedEpoch = startedAt;
  r.startedMs = StartedMs(j, startedAt);
  r.firstFrame = Get<bool>(j, "first_frame", false);

  // The session's own log, which is in the state dir's logs/: a damaged
  // marker naming any other file does not get it copied into a report.
  const std::string log = Get<std::string>(j, "log", "");
  if (!log.empty() && fs::weakly_canonical(fs::path(log).parent_path(), ec) ==
                          fs::weakly_canonical(stateDir / "logs", ec))
    WriteFile(dir / "session.log", Tail(log, kLogTailLines));
  const bool hasDump = fs::exists(dumpFile, ec);
  if (hasDump)
    fs::copy_file(dumpFile, dir / "crash.dmp",
                  fs::copy_options::overwrite_existing, ec);
  r.autosaves = CopyAutosaves(j, startedAt, dir / "autosave");

  const std::string collected = Format(std::time(nullptr), "%Y-%m-%d %H:%M:%S");
  WriteFile(dir / "report.txt", ReportText(r, crashText, hasDump, collected));
  WriteFile(
      dir / "report.json",
      ToJson(r).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) +
          "\n");

  fs::remove(crashFile, ec);
  fs::remove(dumpFile, ec);
  return r;
}

// A report folder's name, "YYYYMMDD-HHMMSS-<pid>" with "-<n>" for the n-th
// report of one session, read as NUMBERS. As text, "-32" sorted ahead of
// "-308" in the same second, so the Windows smoke read the wrong report in a
// fresh Wine prefix (small pids) and PruneReports could delete the newest.
struct StemOrder {
  int64_t startMs = 0; // the session's start; 0 = not known
  int64_t pid = 0;
  int n = 1;
};

std::optional<int64_t> Digits(std::string_view s) {
  if (s.empty() || s.size() > 18)
    return std::nullopt;
  int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9')
      return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return v;
}

StemOrder ReadStem(const std::string& name, int64_t startedMs) {
  StemOrder o;
  o.startMs = startedMs;
  std::vector<std::string_view> parts;
  std::string_view rest(name);
  for (size_t at; (at = rest.find('-')) != std::string_view::npos;
       rest.remove_prefix(at + 1))
    parts.push_back(rest.substr(0, at));
  parts.push_back(rest);
  if (parts.size() < 3 || parts.size() > 4 || parts[0].size() != 8 ||
      parts[1].size() != 6)
    return o;
  const auto date = Digits(parts[0]), time = Digits(parts[1]),
             pid = Digits(parts[2]);
  const auto n =
      parts.size() == 4 ? Digits(parts[3]) : std::optional<int64_t>(1);
  if (!date || !time || !pid || !n || *n > 1000000)
    return o;
  o.pid = *pid;
  o.n = static_cast<int>(*n);
  if (o.startMs == 0) {
    // No start in the report (a damaged or bare folder): the name's second,
    // in the local time it was written in.
    std::tm tm{};
    tm.tm_year = static_cast<int>(*date / 10000) - 1900;
    tm.tm_mon = static_cast<int>(*date / 100 % 100) - 1;
    tm.tm_mday = static_cast<int>(*date % 100);
    tm.tm_hour = static_cast<int>(*time / 10000);
    tm.tm_min = static_cast<int>(*time / 100 % 100);
    tm.tm_sec = static_cast<int>(*time % 100);
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t > 0)
      o.startMs = int64_t{t} * 1000;
  }
  return o;
}

void PruneReports(const fs::path& stateDir, size_t keep) {
  std::error_code ec;
  const std::vector<Report> reports = ListReports(stateDir);
  for (size_t i = keep; i < reports.size(); ++i)
    fs::remove_all(reports[i].dir, ec);
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
  // Before the marker lists it: the unsaved slot is this session's.
  Autosaver::SetInstanceTag(s.stem);

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
  WriteFile(
      dir / "report.json",
      ToJson(r).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) +
          "\n");
  PruneReports(stateDir, kKeepReports);
  LOG_WARN("This session's report ({}) is in {}", reason, dir.string());
  return r;
}

bool NewerReport(const Report& a, const Report& b) {
  const std::string an = a.dir.empty() ? a.stem : a.dir.filename().string();
  const std::string bn = b.dir.empty() ? b.stem : b.dir.filename().string();
  const StemOrder ao = ReadStem(an, a.startedMs);
  const StemOrder bo = ReadStem(bn, b.startedMs);
  if (ao.startMs != bo.startMs)
    return ao.startMs > bo.startMs;
  // One session's reports: the later one has the higher n.
  if (ao.pid == bo.pid && ao.n != bo.n)
    return ao.n > bo.n;
  // Two sessions started in the same millisecond: nothing records which was
  // first, and a pid is not a time. Any order, so long as it is the same one
  // every time.
  if (ao.pid != bo.pid)
    return ao.pid > bo.pid;
  if (ao.n != bo.n)
    return ao.n > bo.n;
  return an > bn;
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
  std::vector<std::string> liveStems;
  for (const fs::path& marker : markers) {
    platform::FileLock lock;
    if (!lock.TryLock(marker, /*create=*/false)) {
      // Somebody holds it: a live session (perhaps this one). Its log and
      // its unsaved slot stay.
      liveLogs.push_back(stateDir / "logs" / (marker.stem().string() + ".log"));
      liveStems.push_back(marker.stem().string());
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
  std::sort(out.begin(), out.end(), NewerReport);
  PruneReports(stateDir, keep);
  logfile::Prune(stateDir / "logs", kKeepLogs, liveLogs);
  // After the reports have their copies: what is left of a dead session's
  // unsaved slot is offered by nothing.
  Autosaver::PruneUnsavedSlots(stateDir.string(), liveStems, kKeepUnsavedSlots);
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
  std::sort(out.begin(), out.end(), NewerReport);
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
