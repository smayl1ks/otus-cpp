#pragma once

#include <iostream>
#include <memory>
#include <string>

#include "models/document.hpp"
#include "models/file.hpp"

#include "view.hpp"

namespace controller
{

class Controller
{
private:
    model::File m_file;
    std::shared_ptr<model::Document> m_document;
    std::shared_ptr<View> m_view;
public:
    Controller()
    {
        m_document = std::make_shared<model::Document>(m_file);
        m_view = std::make_shared<View>();
    }

    void CreateNewDocument()
    {
        std::cout << "[Controller::CreateNewDocument]" << "\n";
        m_document = std::make_shared<model::Document>(m_file);
        UpdateView();
    }

    void ImportDocument(const std::string& path)
    {
        std::cout << "[Controller::ImportDocument] path: " << path << "\n";
        m_document->LoadFromFile(path);
        UpdateView();
    }

    void ExportDocument(const std::string& path)
    {
        std::cout << "[Controller::ExportDocument] path: " << path << "\n";
        m_document->SaveToFile(path);
        UpdateView();
    }

    void AddShape(std::shared_ptr<model::Shape> shape)
    {
        std::cout << "[Controller::AddShape]" << "\n";
        m_document->AddShape(std::move(shape));
        UpdateView();
    }

    void RemoveShape(size_t index)
    {
        std::cout << "[Controller::RemoveShape] index: " << index << "\n";
        m_document->RemoveShape(index);
        UpdateView();
    }

private:
    void UpdateView()
    {
        m_view->render(*m_document);
    }
};

} // namespace controller