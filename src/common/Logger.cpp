// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/Logger.h"

#include "common/LogFile.h"

#include <algorithm>
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>

namespace {

constexpr const char* kPattern = "[%H:%M:%S] [%^%l%$] %v";

// The logger writes to ONE sink, this fan-out, from its first message: sinks
// can be added to a dist_sink under its own mutex while other threads log,
// which the logger's own sink vector does not allow. stdout is in it from the
// start; the session file joins once start-up knows where it goes.
std::shared_ptr<spdlog::sinks::dist_sink_mt>& Fanout() {
  static std::shared_ptr<spdlog::sinks::dist_sink_mt> fanout =
      std::make_shared<spdlog::sinks::dist_sink_mt>();
  return fanout;
}

std::mutex g_fileMutex;
std::shared_ptr<spdlog::sinks::basic_file_sink_mt> g_file;
std::filesystem::path g_filePath;

} // namespace

spdlog::logger* Logger::Instance() {
  // A function-local static: C++ initializes it once, and any other thread
  // arriving meanwhile waits for it to finish.
  static const std::shared_ptr<spdlog::logger> logger = [] {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern(kPattern);
    Fanout()->add_sink(console);
    auto l = std::make_shared<spdlog::logger>("CAD", Fanout());
    l->set_level(spdlog::level::info);
    l->set_pattern(kPattern);
    // Flush every message: when stdout is a pipe/file (CI, remote
    // harnesses), block buffering otherwise swallows the tail of the log on
    // crash or kill. The session file is flushed the same way, for the same
    // reason: it is what a crash report's session.log is cut from.
    l->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(l);
    return l;
  }();
  return logger.get();
}

void Logger::Initialize(spdlog::level::level_enum level) {
  // The logger's level is an atomic in spdlog, so this may run while other
  // threads log. The sink keeps its default (everything): the logger's own
  // level is the one filter.
  spdlog::logger* l = Instance();
  l->set_level(level);
  LOG_INFO("Logger initialized with level: {}",
           spdlog::level::to_string_view(level));
}

namespace logfile {

bool Open(const std::filesystem::path& path, std::string* error) {
  Logger::GetLogger(); // the fan-out exists, with stdout in it
  std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
  try {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(),
                                                               true);
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
  sink->set_pattern(kPattern);
  std::lock_guard lock(g_fileMutex);
  if (g_file)
    Fanout()->remove_sink(g_file);
  Fanout()->add_sink(sink);
  g_file = std::move(sink);
  g_filePath = path;
  return true;
}

std::filesystem::path Current() {
  std::lock_guard lock(g_fileMutex);
  return g_filePath;
}

void Prune(const std::filesystem::path& dir, size_t keep,
           const std::vector<std::filesystem::path>& keepAlways) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> logs;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".log")
      logs.push_back(e.path());
  // Newest first: the name begins with the start time, YYYYmmdd-HHMMSS.
  std::sort(logs.begin(), logs.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename() > b.filename();
  });
  const fs::path current = Current();
  for (size_t i = keep; i < logs.size(); ++i) {
    const bool spared = logs[i] == current ||
                        std::any_of(keepAlways.begin(), keepAlways.end(),
                                    [&](const fs::path& p) {
                                      return p.filename() == logs[i].filename();
                                    });
    if (!spared)
      fs::remove(logs[i], ec);
  }
}

} // namespace logfile
