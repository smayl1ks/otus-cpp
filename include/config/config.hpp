#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "config/defaults.hpp"
#include "config/options.hpp"
#include "config/validator.hpp"

namespace config
{

class ServerConfig
{
private:
    ConfigOptions m_options;
    ConfigData m_config_data;

private:
    static std::unique_ptr<ServerConfig>& Instance();

    void LoadNetworkConfig(const po::variables_map& vm);

    void LoadLoggerConfig(const po::variables_map& vm);

    void LoadSSLConfig(const po::variables_map& vm);

    void LoadStorageConfig(const po::variables_map& vm);

    static std::unique_ptr<ServerConfig>& InstancePtr();

public:
    explicit ServerConfig(int argc, char* argv[]);

    ServerConfig(const ServerConfig&) = delete;
    ServerConfig& operator=(const ServerConfig&) = delete;

    static ServerConfig& Get(int argc = 0, char* argv[] = nullptr);

    const NetworkConfig& GetNetworkSettings() const
    {
        return m_config_data.network;
    }

    const LoggerConfig& GetLoggerSettings() const
    {
        return m_config_data.logger;
    }

    const SSLConfig& GetSSL() const
    {
        return m_config_data.ssl;
    }

    const std::filesystem::path& TmpDir() const
    {
        return m_config_data.tmp_dir;
    }

    bool IsSSLEnabled() const
    {
        return m_config_data.ssl.ssl_run_status;
    }

    unsigned int Threads() const
    {
        return m_config_data.threads;
    }

    static constexpr std::string_view GetServerName()
    {
        return g_ServerName;
    }

    static constexpr std::string_view GetServerVersion()
    {
        return g_ServerVersion;
    }

    std::string GetRootPassword() const;

    /// HMAC key for access tokens. Resolution order (first non-empty wins):
    ///   1. JWT_ACCESS_SECRET env (docker-compose / .env);
    ///   2. RUNNER_ROOT_PASSWORD env (systemd unit: postinst generates it);
    ///   3. --server.root.password option (RUN_SERVER_ROOT_PASSWORD env).
    std::string GetJwtSecret() const;
};

} // namespace config