# UML-диаграммы графического редактора

- **Model** (`include/models/`) — документ и графические примитивы. Не знает ни о View, ни о Controller.
- **View** (`include/view.hpp`) — отображение документа. Знает только интерфейс модели.
- **Controller** (`include/controller.hpp`) — принимает команды от GUI и координирует Model и View.
- **GUI-обработчики** (`src/main.cpp`) — функции `gui::On*`, по одной на каждый элемент интерфейса; собираются в одном файле с `main()`.

## Диаграмма классов

```mermaid
classDiagram
    direction TB

    class ShapeType {
        <<enumeration>>
        Rectangle
        Circle
    }

    class Shape {
        <<abstract>>
        +Draw() const*
        +GetType() ShapeType*
        +Clone() shared_ptr~Shape~*
    }

    class Rectangle {
        -m_width : int
        -m_height : int
        +Rectangle(width, height)
        +Draw() const
        +GetType() ShapeType
        +Clone() shared_ptr~Shape~
    }

    class Circle {
        -m_radius : int
        +Circle(radius)
        +Draw() const
        +GetType() ShapeType
        +Clone() shared_ptr~Shape~
    }

    class File {
        +LoadFromFile(path)
        +SaveToFile(path)
    }

    class Document {
        -m_file : File&
        -m_figures : vector~shared_ptr~Shape~~
        +AddShape(shape)
        +RemoveShape(index)
        +GetShapes() const
        +LoadFromFile(path)
        +SaveToFile(path)
    }

    class View {
        +render(doc)
    }

    class Controller {
        -m_file : File
        -m_document : shared_ptr~Document~
        -m_view : shared_ptr~View~
        +CreateNewDocument()
        +ImportDocument(path)
        +ExportDocument(path)
        +AddShape(shape)
        +RemoveShape(index)
        -UpdateView()
    }

    Shape <|.. Rectangle : реализация
    Shape <|.. Circle : реализация
    Shape .. ShapeType : возвращает
    Document o-- Shape : владеет shared_ptr
    Document --> File : делегирует import/export
    Controller *-- Document : владеет shared_ptr
    Controller *-- View : владеет shared_ptr
    View ..> Document : только читает
```

Владение объектами решено умными указателями: `Controller` владеет `Document` и `View` через `std::shared_ptr`, `Document` владеет примитивами через `std::vector<std::shared_ptr<Shape>>`.

## Последовательность обработки события GUI

```mermaid
sequenceDiagram
    participant GUI as GUI-обработчик (main.cpp)
    participant C as Controller
    participant M as Document (Model)
    participant V as View

    GUI->>C: OnAddRectangle(controller, w, h)
    C->>M: AddShape(make_shared<Rectangle>)
    M->>M: push_back
    C->>V: UpdateView() → render(document)
    V->>V: для каждого примитива shape->Draw()
```

Аналогично устроены остальные обработчики: `OnCreateNewDocument`, `OnImportDocument`, `OnExportDocument`, `OnRemoveShape` — все сведены к вызовам `Controller` и завершаются обновлением представления.
