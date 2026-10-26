#pragma once

#include <filesystem>
#include <fmt/format.h>
#include <string>

namespace config
{

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

struct NetworkConfig
{
    std::string address;
    unsigned short https_port;
    unsigned short http_port;
    unsigned short websocket_port;
};

struct SSLConfig
{
    bool ssl_run_status;
    std::filesystem::path cert_file;
    std::filesystem::path key_file;
};

struct PostgresConfig
{
    std::string host{"localhost"};
    unsigned short port{5432};
    std::string dbname{"run_uram_db"};
    std::string user{"postgres_user"};
    std::string password{""};
    size_t pool_size{0}; // 0 = auto-calculate based on concurrency

    std::string ConnectionString() const
    {
        return fmt::format("host={} port={} dbname={} user={} password={}",
                           host, port, dbname, user, password);
    }
};

struct DevConfig
{
    std::string username;
    std::string password;

    bool HasDevUser() const noexcept
    {
        return !username.empty() && !password.empty();
    }
};

struct AuthConfig
{
    std::string jwt_secret;
    std::string root_password;
};

struct ConfigData
{
    unsigned int threads;
    std::filesystem::path tmp_dir;
    NetworkConfig network;
    LoggerConfig logger;
    SSLConfig ssl;
    PostgresConfig postgres;
    DevConfig dev;
    AuthConfig auth;
};

} // namespace config
