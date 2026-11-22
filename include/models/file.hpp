#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "figures.hpp"

namespace model {

class File {
public:
  void LoadFromFile(const std::string &path,
                    std::vector<std::shared_ptr<Shape>> &shapes) {
    std::cout << "[File::LoadFromFile] path: " << path
              << ", shapes count: " << shapes.size() << "\n";
    // здесь десериализация: shapes.clear() и заполнение из файла
  }
  void SaveToFile(const std::string &path,
                  const std::vector<std::shared_ptr<Shape>> &shapes) {
    std::cout << "[File::SaveToFile] path: " << path
              << ", shapes count: " << shapes.size() << "\n";
    // здесь сериализация shapes в файл
  }
};

} // namespace model