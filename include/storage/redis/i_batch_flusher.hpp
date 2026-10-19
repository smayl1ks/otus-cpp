#pragma once

#include <boost/asio/awaitable.hpp>
#include <vector>

namespace run::storage::redis
{

template <typename T>
class IBatchFlusher
{
public:
    virtual ~IBatchFlusher() = default;
    virtual boost::asio::awaitable<bool> FlushBatchAsync(const std::vector<T>& batch) = 0;
};

} // namespace run::storage::redis
