#pragma once

#include <iostream>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace logger
{

namespace fs = std::filesystem;

struct LoggerConfig
{
    std::string level;
    std::string pattern;
    std::filesystem::path dir;
    size_t max_size;
    size_t max_files;
    int flush_interval;
    bool async;
};

class Logger
{
public:
    using Levels = spdlog::level::level_enum;

    static void Create(const LoggerConfig& config);

    static Levels Level();
    static void SetLevel(Levels level);
};

using Levels = Logger::Levels;

} // namespace logger

#define LOG_TRACE(format, ...)     SPDLOG_TRACE(format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...)     SPDLOG_DEBUG(format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)      SPDLOG_INFO(format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)      SPDLOG_WARN(format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...)     SPDLOG_ERROR(format, ##__VA_ARGS__)
#define LOG_CRITICAL(format, ...)  SPDLOG_CRITICAL(format, ##__VA_ARGS__)