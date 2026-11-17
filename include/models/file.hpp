#pragma once

#include <iostream>
#include <string>

namespace model
{

class File
{
public:
    void LoadFromFile(const std::string &path)
    {
        std::cout << "[File::LoadFromFile] path: " << path << "\n";
    }
    void SaveToFile(const std::string &path)
    {
        std::cout << "[File::SaveToFile] path: " << path << "\n";
    }
};

} // namespace model