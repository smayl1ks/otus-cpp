#include "config/options.hpp"

namespace config
{

ConfigOptions::ConfigOptions()
{
    SetOptions();
}

void ConfigOptions::SetOptions()
{
    SetCommandLineOptions();
    SetCommonOptions();
    SetLoggerOptions();
    SetEnvMapping();
}

void ConfigOptions::SetEnvMapping()
{
    m_environment_options.add_options()("server.root.password", po::value<std::string>(), "Root password");
}

void ConfigOptions::ParseEnvironment()
{
    po::options_description env_opts;
    env_opts.add(m_common_options).add(m_logger_options).add(m_environment_options);

    constexpr std::string_view prefix = "RUN_";

    store(po::parse_environment(env_opts,
                                [&env_opts, prefix](std::string env_var) -> std::string
                                {
                                    if (env_var.rfind(prefix, 0) == 0)
                                    {
                                        std::string opt = env_var.substr(prefix.length()); // cut "RUN_"
                                        std::transform(opt.begin(), opt.end(), opt.begin(), ::tolower);

                                        // Replace '_' to '.' (example: RUN_SERVER_THREADS <=> server.threads)
                                        std::replace(opt.begin(), opt.end(), '_', '.');

                                        if (env_opts.find_nothrow(opt, false) != nullptr)
                                        {
                                            return opt;
                                        }
                                    }
                                    return "";
                                }),
          m_results);
}

void ConfigOptions::SetCommandLineOptions()
{
    m_command_line_options.add_options()("help,h", "display this help message")("version,v", "show server version")(
        "config,c", po::value<std::vector<std::string>>(),
        "path to config file(s)")("dry-run", "validate configuration and exit without starting the server");
}

void ConfigOptions::SetCommonOptions()
{
    m_common_options.add_options()("server.threads", po::value<unsigned int>()->default_value(g_DefaultThreads))(
        "server.tmp-dir", po::value<std::string>()->default_value(g_DefaultTmpDir),
        "Directory for temporary file uploads before processing")(
        "network.address", po::value<std::string>()->default_value(g_DefaultServerAddress), "Server bind address")(
        "network.https.port", po::value<unsigned short>()->default_value(g_DefaultHTTPSPort),
        "HTTPS port")("network.http.port", po::value<unsigned short>()->default_value(g_DefaultHTTPPort), "HTTP port")(
        "network.websocket.port", po::value<unsigned short>()->default_value(g_DefaultWebSocketPort),
        "Websocket port")("ssl.run-ssl", po::value<bool>()->default_value(g_SSLDefaultRunStatus),
                          "Enable SSL")("ssl.cert", po::value<std::string>()->default_value(g_DefaultSSLCertFile),
                                        "Directory for served ssl certificate file")(
        "ssl.key", po::value<std::string>()->default_value(g_DefaultSSLKeyFile), "Directory for served ssl key file");
}

void ConfigOptions::SetLoggerOptions()
{
    m_logger_options.add_options()("logger.dir", po::value<std::string>()->default_value(g_DefaultLoggerDir))(
        "logger.level", po::value<std::string>()->default_value(g_LoggerDefaultLevel),
        "set spdlog level: trace, debug, info, warn, err, critical, off")(
        "logger.pattern", po::value<std::string>()->default_value(g_LoggerDefaultPattern), "spdlog pattern format")(
        "logger.max-size", po::value<size_t>()->default_value(g_LoggerDefaultMaxSize),
        "max log file size in bytes")("logger.max-files", po::value<size_t>()->default_value(g_LoggerDefaultMaxFiles),
                                      "max number of rotated log files")(
        "logger.flush-interval", po::value<int>()->default_value(g_LoggerDefaultFlushInterval),
        "flush logs every N seconds")("logger.async", po::value<bool>()->default_value(false)->implicit_value(true),
                                      "enable asynchronous logging (faster, but may lose last messages on crash)");
}

void ConfigOptions::ParseOptions(int argc, char* argv[])
{
    ParseCommandLine(argc, argv);
    CheckForVersion();
    CheckForHelp();
    ParseEnvironment();
    ParseConfigFiles();
    notify(m_results);
}

void ConfigOptions::ParseCommandLine(int argc, char* argv[])
{
    po::options_description cmd_opts;

    cmd_opts.add(m_command_line_options).add(m_common_options).add(m_logger_options);

    store(po::command_line_parser(argc, argv).options(cmd_opts).run(), m_results);
}

void ConfigOptions::ParseConfigFiles()
{
    if (m_results.count("config"))
    {
        const auto& files = m_results["config"].as<std::vector<std::string>>();
        for (const auto& file : files)
        {
            LoadConfigFile(file);
        }
    }
    else
    {
        ParseDefaultConfigFile();
    }
}

void ConfigOptions::ParseDefaultConfigFile()
{
    LoadConfigFile(g_DefaultConfigFile);
}

void ConfigOptions::LoadConfigFile(std::string_view filename)
{
    po::options_description config_options;
    config_options.add(m_common_options).add(m_logger_options);

    std::filesystem::path config_path{filename};

    std::error_code ec;
    if (std::filesystem::exists(config_path, ec))
    {
        store(po::parse_config_file(filename.data(), config_options, true), m_results);
    }
    else if (ec)
    {
        std::cerr << "OS Error evaluating path: " << ec.message() << "\n";
    }
    else
    {
        std::cerr << "The file definitely does not exist.\n";
    }
}

void ConfigOptions::CheckForVersion() const
{
    if (m_results.count("version"))
    {
        PrintVersion();
    }
}

void ConfigOptions::CheckForHelp() const
{
    if (m_results.count("help"))
    {
        PrintHelp();
    }
}

void ConfigOptions::PrintVersion() const
{
    std::cout << g_ServerName << "\n"
              << "version: " << g_ServerVersion << "\n";

    throw OptionsExitsProgram();
}

void ConfigOptions::PrintHelp() const
{
    std::cout << "\n";
    std::cout << "+----------------------------------------------------------+\n";
    std::cout << "|  " << g_ServerName << "\n";
    std::cout << "|  supported by smayl1ks\n";
    std::cout << "+----------------------------------------------------------+\n\n";

    std::cout << "Usage: runner [options]\n\n";

    po::options_description visible_opts;
    visible_opts.add(m_command_line_options);

    po::options_description common_header("Server Settings");
    common_header.add(m_common_options);
    visible_opts.add(common_header);

    std::cout << visible_opts << std::endl;

    throw OptionsExitsProgram();
}

bool ConfigOptions::IsDryRun() const
{
    return m_results.count("dry-run") > 0;
}

} // namespace config