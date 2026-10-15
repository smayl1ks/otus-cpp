#include "logger/logger.hpp"

namespace
{

constexpr auto g_LoggerName = "Runner";
constexpr auto g_FileName = "runner.log";

} // namespace

namespace logger
{

void Logger::Create(const LoggerConfig& config)
{
    std::vector<spdlog::sink_ptr> sinks;

    try
    {
        if (!fs::exists(config.dir))
        {
            fs::create_directories(config.dir);
        }

        const auto file_path = config.dir / g_FileName;
        const auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file_path.string(), config.max_size, config.max_files);
        sinks.push_back(rotating_sink);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to initialize file logging: " << ex.what() << std::endl;
    }

    if (sinks.empty())
    {
        std::cerr << "Failed to set sinks in spdlog" << std::endl;
        return;
    }

    std::shared_ptr<spdlog::logger> target_logger;

    if (config.async)
    {
        spdlog::init_thread_pool(8192, 1);
        target_logger = std::make_shared<spdlog::async_logger>(
            g_LoggerName, sinks.begin(), sinks.end(), spdlog::thread_pool());
    }
    else
    {
        target_logger = std::make_shared<spdlog::logger>(
            g_LoggerName, sinks.begin(), sinks.end());
    }

    target_logger->set_level(spdlog::level::from_str(config.level));
    target_logger->set_pattern(config.pattern);

    target_logger->flush_on(spdlog::level::err);
    spdlog::flush_every(std::chrono::seconds(config.flush_interval));

    spdlog::set_default_logger(target_logger);
}

void Logger::SetLevel(Levels level)
{
    spdlog::set_level(level);
}

Logger::Levels Logger::Level()
{
    return spdlog::get_level();
}

} // namespace logger