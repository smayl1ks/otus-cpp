#pragma once

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>

#include <chrono>
#include <memory>

namespace run::storage::redis
{

namespace net = boost::asio;

// Boost.Redis 1.90 (the Sans-IO rewrite) has no internal synchronization:
// async_exec mutates multiplexer::reqs_ (add/cancel) inline on the caller's
// thread while the connection's reader/writer tasks touch the same deque from
// the connection's executor. Calling async_exec from any other executor is a
// data race that ends in SIGSEGV inside multiplexer::prepare_write() under
// load. Every command must therefore run on the connection's own executor -
// the strand the connection was created with.
//
// The helpers below co_spawn the command (and its timeout timer, so per-command
// cancellation is delivered there as well) onto conn's executor and hop the
// result back to the caller. Error reporting follows redirect_error semantics:
// no exceptions, ec_out receives the final error code.

template <typename Resp>
net::awaitable<void> ExecSerializedAsync(const std::shared_ptr<boost::redis::connection>& conn,
                                         boost::redis::request& req, Resp& resp,
                                         std::chrono::steady_clock::duration timeout,
                                         boost::system::error_code& ec_out)
{
    auto worker = [&conn, &req, &resp, timeout]() -> net::awaitable<boost::system::error_code> {
        boost::system::error_code ec;
        co_await conn->async_exec(req, resp,
                                  net::cancel_after(timeout, net::redirect_error(net::use_awaitable, ec)));
        co_return ec;
    };
    ec_out = co_await net::co_spawn(conn->get_executor(), std::move(worker), net::use_awaitable);
}

// Same without a per-command timeout - for subscribe-style commands that must
// not be dropped; connection stalls are covered by the health checker.
template <typename Resp>
net::awaitable<void> ExecSerializedAsync(const std::shared_ptr<boost::redis::connection>& conn,
                                         boost::redis::request& req, Resp& resp,
                                         boost::system::error_code& ec_out)
{
    auto worker = [&conn, &req, &resp]() -> net::awaitable<boost::system::error_code> {
        boost::system::error_code ec;
        co_await conn->async_exec(req, resp, net::redirect_error(net::use_awaitable, ec));
        co_return ec;
    };
    ec_out = co_await net::co_spawn(conn->get_executor(), std::move(worker), net::use_awaitable);
}

} // namespace run::storage::redis
