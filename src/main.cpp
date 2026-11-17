#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

#include "controller.hpp"
#include "models/figures.hpp"

namespace gui
{

void OnCreateNewDocument(controller::Controller& controller)
{
    controller.CreateNewDocument();
}

void OnImportDocument(controller::Controller& controller, const std::string& path)
{
    controller.ImportDocument(path);
}

void OnExportDocument(controller::Controller& controller, const std::string& path)
{
    controller.ExportDocument(path);
}

void OnAddRectangle(controller::Controller& controller, int width, int height)
{
    controller.AddShape(std::make_shared<model::Rectangle>(width, height));
}

void OnAddCircle(controller::Controller& controller, int radius)
{
    controller.AddShape(std::make_shared<model::Circle>(radius));
}

void OnRemoveShape(controller::Controller& controller, std::size_t index)
{
    controller.RemoveShape(index);
}

} // namespace gui

namespace
{

constexpr int kDemoRectangleWidth = 10;
constexpr int kDemoRectangleHeight = 20;
constexpr int kDemoCircleRadius = 5;
constexpr int kDemoSmallRectangleWidth = 3;
constexpr int kDemoSmallRectangleHeight = 4;

} // namespace

int main()
{
    try
    {
        controller::Controller controller;

        gui::OnCreateNewDocument(controller);

        gui::OnAddRectangle(controller, kDemoRectangleWidth, kDemoRectangleHeight);
        gui::OnAddCircle(controller, kDemoCircleRadius);
        gui::OnAddRectangle(controller, kDemoSmallRectangleWidth, kDemoSmallRectangleHeight);

        gui::OnExportDocument(controller, "document.editor");

        gui::OnRemoveShape(controller, 0);

        gui::OnImportDocument(controller, "document.editor");
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
