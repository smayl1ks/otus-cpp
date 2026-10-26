#include "config/config.hpp"

#include <cstdlib>

namespace config
{

ServerConfig& ServerConfig::Get(int argc, char* argv[])
{
    auto& inst = InstancePtr();
    if (!inst)
    {
        if (argv == nullptr)
        {
            throw std::runtime_error("ServerConfig must be initialized with argc/argv before first use.");
        }
        inst.reset(new ServerConfig(argc, argv));
    }
    return *inst;
}

std::unique_ptr<ServerConfig>& ServerConfig::InstancePtr()
{
    static std::unique_ptr<ServerConfig> inst = nullptr;
    return inst;
}

ServerConfig::ServerConfig(int argc, char* argv[])
{
    m_options.ParseOptions(argc, argv);

    const auto& vm = m_options.GetResults();

    LoadNetworkConfig(vm);
    LoadLoggerConfig(vm);
    LoadSSLConfig(vm);
    LoadStorageConfig(vm);

    ConfigValidator validator(m_config_data);

    if (!validator.Validate())
    {
        throw std::runtime_error("Configuration validation failed");
    }

    if (m_options.IsDryRun())
    {
        validator.DumpToLog();
        throw OptionsExitsProgram{};
    }
}

void ServerConfig::LoadNetworkConfig(const po::variables_map& vm)
{
    m_config_data.network.address = vm["network.address"].as<std::string>();
    m_config_data.network.https_port = vm["network.https.port"].as<unsigned short>();
    m_config_data.network.http_port = vm["network.http.port"].as<unsigned short>();
    m_config_data.network.websocket_port = vm["network.websocket.port"].as<unsigned short>();
}

void ServerConfig::LoadLoggerConfig(const po::variables_map& vm)
{
    m_config_data.logger.level = vm["logger.level"].as<std::string>();
    m_config_data.logger.pattern = vm["logger.pattern"].as<std::string>();
    m_config_data.logger.dir = vm["logger.dir"].as<std::string>();
    m_config_data.logger.max_size = vm["logger.max-size"].as<size_t>();
    m_config_data.logger.max_files = vm["logger.max-files"].as<size_t>();
    m_config_data.logger.flush_interval = vm["logger.flush-interval"].as<int>();
    m_config_data.logger.async = vm["logger.async"].as<bool>();
}

void ServerConfig::LoadSSLConfig(const po::variables_map& vm)
{
    m_config_data.ssl.ssl_run_status = vm["ssl.run-ssl"].as<bool>();
    m_config_data.ssl.cert_file = vm["ssl.cert"].as<std::string>();
    m_config_data.ssl.key_file = vm["ssl.key"].as<std::string>();
}

void ServerConfig::LoadStorageConfig(const po::variables_map& vm)
{
    m_config_data.threads = vm["server.threads"].as<unsigned int>();
    m_config_data.tmp_dir = vm["server.tmp-dir"].as<std::string>();
}

std::string ServerConfig::GetRootPassword() const
{
    const auto& results = m_options.GetResults();

    if (results.count("server.root.password") == 0)
    {
        return {};
    }

    return results["server.root.password"].as<std::string>();
}

std::string ServerConfig::GetJwtSecret() const
{
    if (const char* secret = std::getenv("JWT_ACCESS_SECRET"); secret != nullptr && secret[0] != '\0')
    {
        return secret;
    }
    // Packaging contract: service/runner.service.in + service/postinst.in.
    if (const char* secret = std::getenv("RUNNER_ROOT_PASSWORD"); secret != nullptr && secret[0] != '\0'
        && std::string_view{secret} != "GENERATED_PLACEHOLDER")
    {
        return secret;
    }
    return GetRootPassword();
}

} // namespace config