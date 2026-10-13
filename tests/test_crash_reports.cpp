// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Crash reports (core/io/CrashReports.h,
// docs/architecture.md#crashes-and-stalls): a session marker is collected
// only once its lock is free, the report says how the session ended, and
// old reports and logs are pruned. tests/crash_report_smoke.py drives the
// same through a real crash.

#include <catch2/catch_test_macros.hpp>

#include "common/Platform.h"
#include "core/io/CrashReports.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

namespace fs = std::filesystem;

namespace {

struct ScratchDir {
  fs::path dir;
  explicit ScratchDir(const char* name)
      : dir(fs::temp_directory_path() / name) {
    fs::remove_all(dir);
    fs::create_directories(dir / "sessions");
    fs::create_directories(dir / "logs");
  }
  ~ScratchDir() { fs::remove_all(dir); }
};

std::string Read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// A marker as a session writes it, for a session that is gone.
void PlantMarker(const fs::path& state, const std::string& stem,
                 const nlohmann::json& extra = nlohmann::json::object()) {
  nlohmann::json j = {{"stem", stem},
                      {"version", "0.0.0"},
                      {"build", "test, Release"},
                      {"mode", "headless"},
                      {"os", "TestOS"},
                      {"started", "2026-01-01 00:00:00"},
                      {"started_epoch", 0},
                      {"log", (state / "logs" / (stem + ".log")).string()},
                      {"project", ""},
                      {"autosaves", nlohmann::json::array()}};
  j.update(extra);
  Write(state / "sessions" / (stem + ".json"), j.dump(2));
}

} // namespace

TEST_CASE("A held session marker is not collected; a released one is",
          "[crash]") {
  ScratchDir s("lobole-crash-lock");
  const fs::path marker = s.dir / "sessions" / "20260101-000000-1.json";
  PlantMarker(s.dir, "20260101-000000-1");

  platform::FileLock held;
  REQUIRE(held.TryLock(marker, /*create=*/false));
  // A second lock in the same process is refused too: the lock is the open
  // file's, which is what keeps a session from collecting itself.
  platform::FileLock second;
  CHECK_FALSE(second.TryLock(marker, false));

  CHECK(crash::CollectDeadSessions(s.dir).empty());
  CHECK(fs::exists(marker));

  held.Release();
  const auto reports = crash::CollectDeadSessions(s.dir);
  REQUIRE(reports.size() == 1);
  CHECK_FALSE(reports[0].crashed);
  CHECK(reports[0].reason.find("ended without a crash report") == 0);
  CHECK_FALSE(fs::exists(marker));
  CHECK(fs::exists(reports[0].dir / "report.txt"));
  // And it is listed, with what it said.
  const auto listed = crash::ListReports(s.dir);
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].reason == reports[0].reason);
  CHECK(listed[0].mode == "headless");
}

TEST_CASE("A crashed session's report holds its stack, log tail and "
          "autosaves",
          "[crash]") {
  ScratchDir s("lobole-crash-collect");
  const std::string stem = "20260102-000000-7";
  std::string log;
  for (int i = 0; i < 2500; ++i)
    log += "line " + std::to_string(i) + "\n";
  Write(s.dir / "logs" / (stem + ".log"), log);
  Write(s.dir / "sessions" / (stem + ".crash"),
        "reason: SIGSEGV\nin: RPC debug_crash\nraw frames (module+offset):\n"
        "  #0 /bin/lobole+0x1234\n");
  // The session's own autosave, and one an earlier session left (older than
  // this one started) that must not be passed off as this session's work.
  const fs::path fresh = s.dir / "work" / "Part.dsgn.autosave";
  const fs::path dir = s.dir / "work" / "Part.dsgn.autosave.d";
  Write(fresh, "{}");
  Write(dir / "parts" / "Stand.json", "{}");
  Write(dir / "parts" / "Old.json", "{}");
  const auto started = std::chrono::system_clock::now() - std::chrono::hours(1);
  fs::last_write_time(dir / "parts" / "Old.json",
                      std::chrono::clock_cast<fs::file_time_type::clock>(
                          started - std::chrono::hours(1)));
  PlantMarker(s.dir, stem,
              {{"project", (s.dir / "work" / "Part.dsgn").string()},
               {"started_epoch", std::chrono::system_clock::to_time_t(started)},
               {"autosaves", {fresh.string(), dir.string()}}});
  Write(s.dir / "sessions" / (stem + ".dmp"), "MDMP");

  const auto reports = crash::CollectDeadSessions(s.dir);
  REQUIRE(reports.size() == 1);
  const crash::Report& r = reports[0];
  CHECK(r.crashed);
  CHECK(r.reason == "crashed: SIGSEGV");
  const std::string text = Read(r.dir / "report.txt");
  CHECK(text.find("Nothing has been sent") != std::string::npos);
  CHECK(text.find("RPC debug_crash") != std::string::npos);
  CHECK(text.find("/bin/lobole+0x1234") != std::string::npos);
  const std::string tail = Read(r.dir / "session.log");
  CHECK(tail.find("line 499\n") == std::string::npos);
  CHECK(tail.rfind("line 500\n", 0) == 0);
  CHECK(tail.find("line 2499\n") != std::string::npos);
  CHECK(Read(r.dir / "crash.dmp") == "MDMP");
  CHECK(fs::exists(r.dir / "autosave" / "Part.dsgn.autosave"));
  CHECK(fs::exists(r.dir / "autosave" / "Part.dsgn.autosave.d" / "parts" /
                   "Stand.json"));
  CHECK_FALSE(fs::exists(r.dir / "autosave" / "Part.dsgn.autosave.d" / "parts" /
                         "Old.json"));
  CHECK(r.autosaves.size() == 2);
  // The session's files are gone; the log stays (it is pruned by count).
  CHECK_FALSE(fs::exists(s.dir / "sessions" / (stem + ".crash")));
  CHECK_FALSE(fs::exists(s.dir / "sessions" / (stem + ".dmp")));
  CHECK_FALSE(fs::exists(s.dir / "sessions" / (stem + ".json")));
}

TEST_CASE("The 21st report deletes the oldest", "[crash]") {
  ScratchDir s("lobole-crash-retention");
  for (int i = 0; i < 20; ++i) {
    char stem[32];
    std::snprintf(stem, sizeof stem, "202501%02d-000000-1", i + 1);
    fs::create_directories(crash::ReportsDir(s.dir) / stem);
  }
  PlantMarker(s.dir, "20260101-000000-9");
  REQUIRE(crash::CollectDeadSessions(s.dir).size() == 1);
  const auto listed = crash::ListReports(s.dir);
  CHECK(listed.size() == crash::kKeepReports);
  CHECK_FALSE(fs::exists(crash::ReportsDir(s.dir) / "20250101-000000-1"));
  CHECK(fs::exists(crash::ReportsDir(s.dir) / "20250102-000000-1"));
  CHECK(listed.front().stem == "20260101-000000-9");
}

TEST_CASE("Old logs are pruned, never a live session's", "[crash]") {
  ScratchDir s("lobole-crash-logs");
  for (int i = 0; i < 12; ++i) {
    char stem[32];
    std::snprintf(stem, sizeof stem, "202501%02d-000000-1", i + 1);
    Write(s.dir / "logs" / (std::string(stem) + ".log"), "x\n");
  }
  // The OLDEST log belongs to a session that is still running.
  PlantMarker(s.dir, "20250101-000000-1");
  platform::FileLock live;
  REQUIRE(live.TryLock(s.dir / "sessions" / "20250101-000000-1.json", false));
  CHECK(crash::CollectDeadSessions(s.dir).empty());
  size_t logs = 0;
  for (const auto& e : fs::directory_iterator(s.dir / "logs"))
    logs += e.path().extension() == ".log";
  CHECK(logs == crash::kKeepLogs + 1);
  CHECK(fs::exists(s.dir / "logs" / "20250101-000000-1.log"));
  CHECK_FALSE(fs::exists(s.dir / "logs" / "20250102-000000-1.log"));
  CHECK(fs::exists(s.dir / "logs" / "20250112-000000-1.log"));
}

TEST_CASE("A session's own marker is held for its life and gone after",
          "[crash]") {
  ScratchDir s("lobole-crash-session");
  const auto session = crash::BeginSession(s.dir, "headless");
  REQUIRE(session.has_value());
  CHECK(fs::exists(session->marker));
  // It is live: a collector (this process, or another starting now) leaves
  // it alone.
  CHECK(crash::CollectDeadSessions(s.dir).empty());
  CHECK(fs::exists(session->marker));
  crash::NoteProject("/some/where/Part.dsgn");
  crash::EndSession();
  CHECK_FALSE(fs::exists(session->marker));
  CHECK(crash::CollectDeadSessions(s.dir).empty());
}
