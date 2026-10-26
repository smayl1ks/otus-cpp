#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <thread>

namespace run::context
{

class WorkContext
{
private:
    boost::asio::thread_pool m_thread_pool;

public:
    explicit WorkContext(unsigned int threads = std::thread::hardware_concurrency());
    ~WorkContext();

    WorkContext(const WorkContext&) = delete;
    WorkContext& operator=(const WorkContext&) = delete;

    void Stop();

    boost::asio::thread_pool& GetThreadPool() noexcept
    {
        return m_thread_pool;
    }

    boost::asio::thread_pool::executor_type GetExecutor() noexcept
    {
        return m_thread_pool.get_executor();
    }

    template <typename F>
    void Post(F&& f)
    {
        boost::asio::post(m_thread_pool, std::forward<F>(f));
    }

    template <typename F>
    auto AsyncPost(F&& f)
    {
        return boost::asio::co_spawn(
            m_thread_pool.get_executor(),
            [f = std::forward<F>(f)]() mutable -> boost::asio::awaitable<std::invoke_result_t<F>> { co_return f(); },
            boost::asio::use_awaitable);
    }
};

} // namespace run::context