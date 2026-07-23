#include "common/Logger.h"

std::shared_ptr<spdlog::logger> Logger::s_logger;

void Logger::Initialize(spdlog::level::level_enum level) {
    // Create a color console sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(level);
    
    // Create logger with custom pattern
    s_logger = std::make_shared<spdlog::logger>("CAD", console_sink);
    s_logger->set_level(level);
    s_logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    
    // Set as default logger
    spdlog::set_default_logger(s_logger);
    
    LOG_INFO("Logger initialized with level: {}", spdlog::level::to_string_view(level));
}