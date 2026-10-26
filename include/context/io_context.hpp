#pragma once

#include <thread>
#include <vector>

#include "common.hpp"

namespace run::context
{

class IOContext
{
private:
    net::io_context m_io_context;
    unsigned int m_thread_pool_size;
    net::executor_work_guard<net::io_context::executor_type> m_work_guard;

public:
    explicit IOContext(unsigned int thread_pool_size = std::thread::hardware_concurrency());

    IOContext(const IOContext&) = delete;
    IOContext& operator=(const IOContext&) = delete;

    void Run();
    void Stop();

    net::io_context& GetIOContext() noexcept
    {
        return m_io_context;
    }
};

} // namespace run::context