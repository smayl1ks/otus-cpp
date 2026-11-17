#pragma once

#include <iostream>
#include <memory>

namespace model
{

enum class ShapeType
{
    Rectangle,
    Circle
};

class Shape
{
public:
    virtual void Draw() const = 0;
    [[nodiscard]] virtual ShapeType GetType() const = 0;
    [[nodiscard]] virtual std::shared_ptr<Shape> Clone() const = 0;

    virtual ~Shape() = default;
};

class Rectangle : public Shape
{
private:
    int m_width{0};
    int m_height{0};

public:
    explicit Rectangle(int width, int height) : m_width{width}, m_height{height} {}
    Rectangle(const Rectangle& other) = default;

    void Draw() const override
    {
        std::cout << "Rectangle drawing with width: " << m_width << " and height: " << m_height << "\n";
    }
    [[nodiscard]] ShapeType GetType() const override
    {
        return ShapeType::Rectangle;
    }
    [[nodiscard]] std::shared_ptr<Shape> Clone() const override
    {
        return std::make_shared<Rectangle>(*this);
    }
};

class Circle : public Shape
{
private:
    int m_radius{0};
public:
    explicit Circle(int radius) : m_radius{radius} {}
    Circle(const Circle& other) = default;

    void Draw() const override
    {
        std::cout << "Circle drawing with radius: " << m_radius << "\n";
    }

    [[nodiscard]] ShapeType GetType() const override
    {
        return ShapeType::Circle;
    }

    [[nodiscard]] std::shared_ptr<Shape> Clone() const override
    {
        return std::make_shared<Circle>(*this);
    }
};

} // namespace model

