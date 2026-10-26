#pragma once

#include <filesystem>
#include <iostream>
#include <string>

#include <boost/program_options.hpp>

#include "config/defaults.hpp"

namespace config
{

class OptionsExitsProgram final : public std::exception
{
};

namespace po = boost::program_options;

class ConfigOptions
{
private:
    po::options_description m_command_line_options;
    po::options_description m_common_options;
    po::options_description m_logger_options;
    po::options_description m_environment_options;

    po::variables_map m_results;

private:
    void SetOptions();

    void SetCommonOptions();
    void SetCommandLineOptions();
    void SetLoggerOptions();
    void SetEnvMapping();

    void ParseCommandLine(int argc, char* argv[]);
    void ParseConfigFiles();
    void ParseDefaultConfigFile();
    void ParseEnvironment();

    void LoadConfigFile(std::string_view filename);

    void CheckForVersion() const;
    void CheckForHelp() const;

    void PrintVersion() const;
    void PrintHelp() const;

public:
    explicit ConfigOptions();

    ConfigOptions(const ConfigOptions&) = delete;
    ConfigOptions& operator=(const ConfigOptions&) = delete;
    ConfigOptions(ConfigOptions&&) noexcept = default;

    void ParseOptions(int argc, char* argv[]);

    const po::variables_map& GetResults() const
    {
        return m_results;
    }

    bool IsDryRun() const;
};

} // namespace config