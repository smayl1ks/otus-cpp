#pragma once

#include <iostream>

#include "models/document.hpp"

class View
{
public:
    void render(const model::Document& doc)
    {
        std::cout << "[View::render] shapes count: " << doc.GetShapes().size() << "\n";
        for (const auto& shape : doc.GetShapes())
        {
            shape->Draw();
        }
    }

};