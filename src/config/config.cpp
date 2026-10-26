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
    LoadDatabaseConfig(vm);
    LoadDevConfig(vm);
    LoadAuthConfig(vm);

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

void ServerConfig::LoadDatabaseConfig(const po::variables_map& vm)
{
    if (vm.count("postgres.host"))
        m_config_data.postgres.host = vm["postgres.host"].as<std::string>();
    if (vm.count("postgres.port"))
        m_config_data.postgres.port = vm["postgres.port"].as<unsigned short>();
    if (vm.count("postgres.db"))
        m_config_data.postgres.dbname = vm["postgres.db"].as<std::string>();
    if (vm.count("postgres.user"))
        m_config_data.postgres.user = vm["postgres.user"].as<std::string>();
    if (vm.count("postgres.password"))
        m_config_data.postgres.password = vm["postgres.password"].as<std::string>();
    if (vm.count("postgres.pool-size"))
        m_config_data.postgres.pool_size = vm["postgres.pool-size"].as<size_t>();
}

void ServerConfig::LoadDevConfig(const po::variables_map& vm)
{
    if (vm.count("dev.username"))
        m_config_data.dev.username = vm["dev.username"].as<std::string>();
    if (vm.count("dev.password"))
        m_config_data.dev.password = vm["dev.password"].as<std::string>();
}

void ServerConfig::LoadAuthConfig(const po::variables_map& vm)
{
    if (vm.count("auth.jwt-secret"))
        m_config_data.auth.jwt_secret = vm["auth.jwt-secret"].as<std::string>();
    if (vm.count("server.root.password"))
        m_config_data.auth.root_password = vm["server.root.password"].as<std::string>();
}

std::string ServerConfig::GetRootPassword() const
{
    return m_config_data.auth.root_password;
}

std::string ServerConfig::GetJwtSecret() const
{
    if (!m_config_data.auth.jwt_secret.empty() && m_config_data.auth.jwt_secret != "GENERATED_PLACEHOLDER")
    {
        return m_config_data.auth.jwt_secret;
    }
    return GetRootPassword();
}

} // namespace config
