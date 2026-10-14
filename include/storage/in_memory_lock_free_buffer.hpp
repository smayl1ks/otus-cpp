#pragma once

#include <boost/lockfree/queue.hpp>
#include <memory>

namespace run::storage
{

template <typename T>
class InMemoryLockFreeBuffer
{
public:
    using ElementPtr = std::shared_ptr<T>;

    explicit InMemoryLockFreeBuffer(std::size_t capacity = 8192) : m_queue(capacity)
    {
    }

    ~InMemoryLockFreeBuffer() = default;

    InMemoryLockFreeBuffer(const InMemoryLockFreeBuffer&) = delete;
    InMemoryLockFreeBuffer& operator=(const InMemoryLockFreeBuffer&) = delete;
    InMemoryLockFreeBuffer(InMemoryLockFreeBuffer&&) = delete;
    InMemoryLockFreeBuffer& operator=(InMemoryLockFreeBuffer&&) = delete;

    bool Push(ElementPtr item) noexcept
    {
        return m_queue.push(std::move(item));
    }

    bool Pop(ElementPtr& out_item) noexcept
    {
        return m_queue.pop(out_item);
    }

    bool IsEmpty() const noexcept
    {
        return m_queue.empty();
    }

private:
    boost::lockfree::queue<ElementPtr> m_queue;
};

} // namespace run::storage