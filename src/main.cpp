#include "config/config.hpp"
#include "logger/logger.hpp"
#include "network/server/server.hpp"

#include <filesystem>

int main(int argc, char* argv[])
{
    const auto& server_config = config::ServerConfig::Get(argc, argv);

    logger::LoggerConfig log_config;
    log_config.level = server_config.GetLoggerSettings().level;
    log_config.pattern = server_config.GetLoggerSettings().pattern;
    log_config.dir = server_config.GetLoggerSettings().dir;
    log_config.max_size = server_config.GetLoggerSettings().max_size;
    log_config.max_files = server_config.GetLoggerSettings().max_files;
    log_config.flush_interval = server_config.GetLoggerSettings().flush_interval;
    log_config.async = server_config.GetLoggerSettings().async;
    logger::Logger::Create(log_config);

    LOG_INFO("Starting {} v{}", server_config.GetServerName(), server_config.GetServerVersion());

    run::server::Server server(server_config.Threads(), std::thread::hardware_concurrency() / 2 + 1);
    server.Run();

    LOG_INFO("Server stopped");
    return 0;
}
