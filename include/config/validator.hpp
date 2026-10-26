#pragma once

#include "config/defaults.hpp"

#include "config/data.hpp"

#include "logger/logger.hpp"

namespace config
{

class ConfigValidator
{
private:
    const ConfigData& m_config_data;

    bool ValidateServerDirs() const;
    bool ValidateServerFiles() const;

public:
    ConfigValidator(const ConfigData& config_data) : m_config_data(config_data)
    {
    }

    bool Validate() const;
    void DumpToLog() const;
};

} // namespace config