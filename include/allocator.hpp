#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

template <typename T>
struct AllocatorPool
{
public:
    std::size_t limit{0};
    std::size_t allocated{0};
    T* storage{nullptr};

    explicit AllocatorPool(std::size_t lim)
        : limit(lim)
    {
    }

    ~AllocatorPool()
    {
        if (storage)
        {
            ::operator delete(storage, std::align_val_t{alignof(T)});
        }
    }

    T* allocate(std::size_t n)
    {
        if (n == 0)
        {
            return nullptr;
        }

        if (allocated + n > limit)
        {
            throw std::bad_alloc();
        }

        if (!storage)
        {
            storage = static_cast<T*>(::operator new(limit * sizeof(T), std::align_val_t{alignof(T)}));
        }

        T* result = storage + allocated;
        allocated += n;
        return result;
    }

    void deallocate(T*, std::size_t) noexcept
    {
        // аллокатор освобождает всю память самостоятельно в деструкторе пула
    }
};

template <typename T, std::size_t N>
class CustomAllocator
{
public:
    using value_type = T;

    CustomAllocator()
        : m_pool(std::make_shared<AllocatorPool<T>>(N))
    {
    }

    template <typename U>
    CustomAllocator(const CustomAllocator<U, N>&)
        : m_pool(std::make_shared<AllocatorPool<T>>(N))
    {
    }

    template <typename U>
    struct rebind
    {
        using other = CustomAllocator<U, N>;
    };

    T* allocate(std::size_t n)
    {
        return m_pool->allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept
    {
        m_pool->deallocate(p, n);
    }

    template <typename U>
    bool operator==(const CustomAllocator<U, N>& other) const noexcept
    {
        if constexpr (std::is_same_v<T, U>)
        {
            return m_pool == other.m_pool;
        }
        else
        {
            return false;
        }
    }

    template <typename U>
    bool operator!=(const CustomAllocator<U, N>& other) const noexcept
    {
        return !(*this == other);
    }

private:
    std::shared_ptr<AllocatorPool<T>> m_pool;
    template <typename U, std::size_t M>
    friend class CustomAllocator;
};
