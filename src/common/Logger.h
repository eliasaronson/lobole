// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

class Logger {
public:
  // Sets the level. Safe from any thread, at any time: it adjusts the one
  // logger rather than replacing it.
  static void Initialize(spdlog::level::level_enum level = spdlog::level::info);
  // The logger is built on first use, so library code (core, tests, scripts)
  // can log without depending on application startup -- and built exactly
  // once. It used to be a shared_ptr assigned on first use, and a worker that
  // logged first raced the main thread building it: each read a pointer the
  // other was writing, and the logger one of them built could be freed under
  // the other.
  static spdlog::logger* GetLogger() { return Instance(); }

private:
  static spdlog::logger* Instance();
};

// Convenient macros for logging
#define LOG_TRACE(...) ::Logger::GetLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::Logger::GetLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...) ::Logger::GetLogger()->info(__VA_ARGS__)
#define LOG_WARN(...) ::Logger::GetLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) ::Logger::GetLogger()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::Logger::GetLogger()->critical(__VA_ARGS__)
