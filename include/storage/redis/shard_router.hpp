#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

namespace run::redis
{

template <typename T>
concept RoutingKey = std::integral<T> || std::convertible_to<T, std::string_view>;

class ShardRouter
{

public:
    explicit ShardRouter(std::size_t num_shards) : m_num_shards{num_shards}
    {
    }

public:
    std::size_t GetShardIndex(uint64_t key) const noexcept
    {
        return Hash64(key) % m_num_shards;
    }

    // string hash with support Redis Hash Tags {...} (like Redis Cluster)
    std::size_t GetShardIndex(std::string_view key) const noexcept
    {
        auto start = key.find('{');
        if (start != std::string_view::npos)
        {
            auto end = key.find('}', start + 1);
            if (end != std::string_view::npos && end > start + 1)
            {
                key = key.substr(start + 1, end - start - 1);
            }
        }

        return HashString(key) % m_num_shards;
    }

    // Fast 64-bit mixer (SplitMix64)
    static uint64_t Hash64(uint64_t x) noexcept
    {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    // FNV-1a 64-bit string hash
    static uint64_t HashString(std::string_view str) noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        for (const char c : str)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

private:
    std::size_t m_num_shards;
};

} // namespace run::redis