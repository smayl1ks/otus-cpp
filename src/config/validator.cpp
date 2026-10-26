#include "config/validator.hpp"

#include <array>

namespace config
{

constexpr auto g_DefaultServerDirs = std::to_array<std::string_view>(
    {g_DefaultServerDir, g_DefaultSSLDir, g_DefaultConfigDir, g_DefaultLoggerDir, g_DefaultTmpDir});

} // namespace config

namespace config
{

bool ConfigValidator::Validate() const
{
    if (!ValidateServerDirs())
    {
        return false;
    }

    return true;
}

bool ConfigValidator::ValidateServerDirs() const
{
    // Validate the CONFIGURED locations, not the compile-time defaults:
    // every path is overridable from the command line / config file, and the
    // defaults under /opt, /var, /etc are often not writable in dev.
    const std::array<std::filesystem::path, 2> required_dirs = {m_config_data.tmp_dir, m_config_data.logger.dir};

    try
    {
        for (const auto& dir : required_dirs)
        {
            if (!std::filesystem::exists(dir))
            {
                if (!std::filesystem::create_directories(dir))
                {
                    LOG_ERROR("Cannot create configured directory: {}", dir.string());
                    return false;
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        LOG_ERROR("Filesystem error while validating server dirs: {}", e.what());
        return false;
    }

    return true;
}

bool ConfigValidator::ValidateServerFiles() const
{
    if (m_config_data.ssl.ssl_run_status)
    {
        if (!std::filesystem::exists(m_config_data.ssl.cert_file))
        {
            return false;
        }
        if (!std::filesystem::exists(m_config_data.ssl.key_file))
        {
            return false;
        }
    }

    return true;
}

void ConfigValidator::DumpToLog() const
{
    LOG_INFO("--- CONFIGURATION ---");
    LOG_INFO("Threads: {}", m_config_data.threads);
    LOG_INFO("Temp directory: {}", m_config_data.tmp_dir.string());

    LOG_INFO("Network address: {}", m_config_data.network.address);
    LOG_INFO("HTTP port: {}", m_config_data.network.http_port);
    LOG_INFO("HTTPS port: {}", m_config_data.network.https_port);
    LOG_INFO("Websocket port: {}", m_config_data.network.websocket_port);
    LOG_INFO("SSL status: {}", m_config_data.ssl.ssl_run_status ? "ON" : "OFF");

    if (m_config_data.ssl.ssl_run_status)
    {
        LOG_INFO("SSL certificate file: {}", m_config_data.ssl.cert_file.string());
        LOG_INFO("SSL key file: {}", m_config_data.ssl.key_file.string());
    }

    LOG_INFO("Logger level: {}", m_config_data.logger.level);
    LOG_INFO("Logger pattern: {}", m_config_data.logger.pattern);
    LOG_INFO("Logger directory: {}", m_config_data.logger.dir.string());
    LOG_INFO("Logger max size: {} bytes", m_config_data.logger.max_size);
    LOG_INFO("Logger max files: {}", m_config_data.logger.max_files);
    LOG_INFO("Logger flush interval: {} seconds", m_config_data.logger.flush_interval);

    LOG_INFO("--- END CONFIGURATION ---");
}

} // namespace config