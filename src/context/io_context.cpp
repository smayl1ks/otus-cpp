#include "context/io_context.hpp"

namespace run::context
{

IOContext::IOContext(unsigned int thread_pool_size)
    : m_io_context(static_cast<int>(thread_pool_size)),
      m_thread_pool_size(thread_pool_size == 0 ? 1 : thread_pool_size), m_work_guard(net::make_work_guard(m_io_context))
{
}

void IOContext::Run()
{
    std::vector<std::thread> threads;
    threads.reserve(m_thread_pool_size > 0 ? m_thread_pool_size - 1 : 0);

    for (std::size_t i = 1; i < m_thread_pool_size; ++i)
    {
        threads.emplace_back([this]() { m_io_context.run(); });
    }

    m_io_context.run();

    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
}

void IOContext::Stop()
{
    m_work_guard.reset();
    m_io_context.stop();
}

} // namespace run::context