#pragma once

#include <cstddef>
#include <memory>
#include <new>

class AllocatorPool
{
public:
    explicit AllocatorPool(std::size_t limit_bytes)
        : m_limit(limit_bytes)
        , m_storage(static_cast<std::byte*>(::operator new(limit_bytes)))
    {
    }

    AllocatorPool(const AllocatorPool&) = delete;
    AllocatorPool& operator=(const AllocatorPool&) = delete;

    ~AllocatorPool()
    {
        ::operator delete(m_storage);
    }

    void* allocate(std::size_t bytes, std::size_t alignment)
    {
        if (bytes == 0)
        {
            return nullptr;
        }

        void* request = m_storage + m_allocated;
        std::size_t available = m_limit - m_allocated;
        void* result = std::align(alignment, bytes, request, available);
        if (!result)
        {
            throw std::bad_alloc();
        }

        m_allocated = m_limit - available + bytes;
        return result;
    }

    void deallocate(void*, std::size_t) noexcept
    {
    }

private:
    std::size_t m_limit;
    std::size_t m_allocated{0};
    std::byte* m_storage{nullptr};
};

struct PoolHolder
{
    std::shared_ptr<AllocatorPool> pool;
};

template <typename T, std::size_t N>
class CustomAllocator
{
public:
    using value_type = T;

    CustomAllocator()
        : m_pool(std::make_shared<PoolHolder>())
    {
    }

    template <typename U>
    CustomAllocator(const CustomAllocator<U, N>& other) noexcept
        : m_pool(other.m_pool)
    {
    }

    template <typename U>
    struct rebind
    {
        using other = CustomAllocator<U, N>;
    };

    T* allocate(std::size_t n)
    {
        if (!m_pool->pool)
        {
            m_pool->pool = std::make_shared<AllocatorPool>(N * sizeof(T));
        }
        return static_cast<T*>(m_pool->pool->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept
    {
        m_pool->pool->deallocate(p, n * sizeof(T));
    }

    template <typename U>
    bool operator==(const CustomAllocator<U, N>& other) const noexcept
    {
        return m_pool == other.m_pool;
    }

    template <typename U>
    bool operator!=(const CustomAllocator<U, N>& other) const noexcept
    {
        return !(*this == other);
    }

private:
    std::shared_ptr<PoolHolder> m_pool;
    template <typename U, std::size_t M>
    friend class CustomAllocator;
};
