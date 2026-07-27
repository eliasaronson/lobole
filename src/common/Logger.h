#pragma once

#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

class Logger {
public:
  static void Initialize(spdlog::level::level_enum level = spdlog::level::info);
  // Lazily initializes so library code (core, tests, scripts) can log
  // without depending on application startup.
  static std::shared_ptr<spdlog::logger>& GetLogger() {
    if (!s_logger)
      Initialize();
    return s_logger;
  }

private:
  static std::shared_ptr<spdlog::logger> s_logger;
};

// Convenient macros for logging
#define LOG_TRACE(...) ::Logger::GetLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::Logger::GetLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...) ::Logger::GetLogger()->info(__VA_ARGS__)
#define LOG_WARN(...) ::Logger::GetLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) ::Logger::GetLogger()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::Logger::GetLogger()->critical(__VA_ARGS__)