#pragma once

#include <boost/asio.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace net = boost::asio;

namespace run::repository
{

// Key-value plane for short-lived credentials (WS tickets) and hot caches.
// Production impl: Redis; test/dev impl: in-memory (in_memory_auth_stores.hpp).
class IRedisRepository
{
public:
    virtual ~IRedisRepository() = default;

    virtual net::awaitable<bool> Set(const std::string& key, const std::string& value, std::chrono::seconds ttl) = 0;

    virtual net::awaitable<std::optional<std::string>> Get(const std::string& key) = 0;

    virtual net::awaitable<bool> Delete(const std::string& key) = 0;
};

} // namespace run::repository
