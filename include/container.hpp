#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>

template <typename T, typename Allocator = std::allocator<T>>
class CustomContainer
{
private:
    struct Node
    {
        T value;
        Node* next{nullptr};

        template <typename... Args>
        Node(Node* next_node, Args&&... args)
            : value(std::forward<Args>(args)...)
            , next(next_node)
        {
        }
    };

    using NodeAlloc = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeTraits = std::allocator_traits<NodeAlloc>;

    [[no_unique_address]] NodeAlloc m_alloc;
    Node* m_head{nullptr};
    Node* m_tail{nullptr};
    std::size_t m_size{0};

public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using reference = T&;
    using const_reference = const T&;

    CustomContainer() = default;

    explicit CustomContainer(const Allocator& alloc)
        : m_alloc(alloc)
    {
    }

    ~CustomContainer()
    {
        clear();
    }

    CustomContainer(const CustomContainer& other)
        : m_alloc(NodeTraits::select_on_container_copy_construction(other.m_alloc))
    {
        for (const auto& item : other)
        {
            push(item);
        }
    }

    CustomContainer& operator=(const CustomContainer& other)
    {
        if (this != &other)
        {
            clear();
            if constexpr (NodeTraits::propagate_on_container_copy_assignment::value)
            {
                m_alloc = other.m_alloc;
            }
            for (const auto& item : other)
            {
                push(item);
            }
        }
        return *this;
    }

    CustomContainer(CustomContainer&& other) noexcept
        : m_alloc(std::move(other.m_alloc))
        , m_head(other.m_head)
        , m_tail(other.m_tail)
        , m_size(other.m_size)
    {
        other.m_head = nullptr;
        other.m_tail = nullptr;
        other.m_size = 0;
    }

    CustomContainer& operator=(CustomContainer&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            if constexpr (NodeTraits::propagate_on_container_move_assignment::value)
            {
                m_alloc = std::move(other.m_alloc);
            }
            m_head = other.m_head;
            m_tail = other.m_tail;
            m_size = other.m_size;
            other.m_head = nullptr;
            other.m_tail = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    template <typename... Args>
    void emplace_back(Args&&... args)
    {
        Node* node = NodeTraits::allocate(m_alloc, 1);
        try
        {
            NodeTraits::construct(m_alloc, node, nullptr, std::forward<Args>(args)...);
        }
        catch (...)
        {
            NodeTraits::deallocate(m_alloc, node, 1);
            throw;
        }

        if (!m_head)
        {
            m_head = m_tail = node;
        }
        else
        {
            m_tail->next = node;
            m_tail = node;
        }
        ++m_size;
    }

    void push(const T& value)
    {
        emplace_back(value);
    }

    void push(T&& value)
    {
        emplace_back(std::move(value));
    }

    void clear() noexcept
    {
        Node* curr = m_head;
        while (curr)
        {
            Node* next = curr->next;
            NodeTraits::destroy(m_alloc, curr);
            NodeTraits::deallocate(m_alloc, curr, 1);
            curr = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_size = 0;
    }

    size_type size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }

    template <typename ItemType, typename NodeType>
    class ForwardIterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ItemType;
        using difference_type = std::ptrdiff_t;
        using pointer = ItemType*;
        using reference = ItemType&;

        ForwardIterator() = default;
        explicit ForwardIterator(NodeType* node) : m_node(node) {}

        reference operator*() const noexcept { return m_node->value; }
        pointer operator->() const noexcept { return std::addressof(m_node->value); }

        ForwardIterator& operator++() noexcept
        {
            if (m_node)
            {
                m_node = m_node->next;
            }
            return *this;
        }

        ForwardIterator operator++(int) noexcept
        {
            ForwardIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(const ForwardIterator& a, const ForwardIterator& b) noexcept
        {
            return a.m_node == b.m_node;
        }

        friend bool operator!=(const ForwardIterator& a, const ForwardIterator& b) noexcept
        {
            return a.m_node != b.m_node;
        }

    private:
        NodeType* m_node{nullptr};
    };

    using iterator = ForwardIterator<T, Node>;
    using const_iterator = ForwardIterator<const T, const Node>;

    iterator begin() noexcept { return iterator(m_head); }
    iterator end() noexcept { return iterator(nullptr); }
    const_iterator begin() const noexcept { return const_iterator(m_head); }
    const_iterator end() const noexcept { return const_iterator(nullptr); }
    const_iterator cbegin() const noexcept { return const_iterator(m_head); }
    const_iterator cend() const noexcept { return const_iterator(nullptr); }
};
