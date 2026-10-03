#include "common/Logger.h"

spdlog::logger* Logger::Instance() {
  // A function-local static: C++ initializes it once, and any other thread
  // arriving meanwhile waits for it to finish.
  static const std::shared_ptr<spdlog::logger> logger = [] {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto l = std::make_shared<spdlog::logger>("CAD", sink);
    l->set_level(spdlog::level::info);
    l->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    // Flush every message: when stdout is a pipe/file (CI, remote
    // harnesses), block buffering otherwise swallows the tail of the log on
    // crash or kill.
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
