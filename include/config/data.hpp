#pragma once

#include <filesystem>
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

struct ConfigData
{
    unsigned int threads;
    std::filesystem::path tmp_dir;
    NetworkConfig network;
    LoggerConfig logger;
    SSLConfig ssl;
};

} // namespace config