#pragma once

#include <cstddef>

#include "version.hpp"

namespace config
{

constexpr auto g_ServerName = "Run Server - runner";

constexpr auto g_ServerVersion = PROJECT_VERSION;

constexpr auto g_DefaultServerDir = "/opt/runner/";
constexpr auto g_DefaultTmpDir = "/tmp/runner/";
constexpr auto g_DefaultLoggerDir = "/var/log/runner/";
constexpr auto g_DefaultSSLDir = "/opt/runner/ssl";
constexpr auto g_DefaultConfigDir = "/etc/runner/";

constexpr auto g_SSLDefaultRunStatus = false;
constexpr auto g_DefaultSSLCertFile = "/opt/runner/ssl/cert.pem";
constexpr auto g_DefaultSSLKeyFile = "/opt/runner/ssl/key.pem";

constexpr auto g_DefaultConfigFile = "/etc/runner/runner.cfg";

constexpr int g_DefaultThreads = 4;

constexpr auto g_DefaultServerAddress = "0.0.0.0";
constexpr unsigned short g_DefaultHTTPSPort = 8080;
constexpr unsigned short g_DefaultHTTPPort = 8081;
constexpr unsigned short g_DefaultWebSocketPort = 9090;

constexpr auto g_LoggerDefaultLevel = "info";
constexpr auto g_LoggerDefaultPattern = "[%Y-%m-%d %H:%M:%S.%f] [%l] %v";
constexpr size_t g_LoggerDefaultMaxSize = 5 * 1024 * 1024; // 5 MB
constexpr size_t g_LoggerDefaultMaxFiles = 3;
constexpr int g_LoggerDefaultFlushInterval = 3; // seconds

constexpr auto g_DefaultPostgresHost = "localhost";
constexpr unsigned short g_DefaultPostgresPort = 5432;
constexpr auto g_DefaultPostgresDB = "run_uram_db";
constexpr auto g_DefaultPostgresUser = "postgres_user";
constexpr auto g_DefaultPostgresPassword = "";
constexpr size_t g_DefaultPostgresPoolSize = 0;

} // namespace config
