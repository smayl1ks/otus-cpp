#pragma once

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "figures.hpp"
#include "file.hpp"

namespace model
{

class Document
{
public:
    explicit Document(File& file) : m_file{file} {}

    void AddShape(const std::shared_ptr<Shape>& shape)
    {
        m_figures.push_back(shape);
        std::cout << "[Document::AddShape] shapes count: " << m_figures.size() << "\n";
    }
    void RemoveShape(size_t index)
    {
        if (index < m_figures.size())
        {
            m_figures.erase(m_figures.begin() + static_cast<std::ptrdiff_t>(index));
            std::cout << "[Document::RemoveShape] removed index: " << index
                      << ", shapes count: " << m_figures.size() << "\n";
        }
        else
        {
            std::cout << "[Document::RemoveShape] index out of range: " << index << "\n";
        }
    }

    [[nodiscard]] const std::vector<std::shared_ptr<Shape>>& GetShapes() const
    {
        return m_figures;
    }

    void LoadFromFile(const std::string& path)
    {
        std::cout << "[Document::LoadFromFile] path: " << path << "\n";
        m_file.LoadFromFile(path);
    }

    void SaveToFile(const std::string& path)
    {
        std::cout << "[Document::SaveToFile] path: " << path << "\n";
        m_file.SaveToFile(path);
    }

private:
    File& m_file;
    std::vector<std::shared_ptr<Shape>> m_figures;

};

} // namespace model