#pragma once

#include <functional>
#include <memory>
#include <thread>

#include "context/io_context.hpp"
#include "context/ssl_context.hpp"
#include "context/work_context.hpp"

#include "logger/logger.hpp"

namespace run::context
{

class Context
{
private:
    IOContext m_io_context;
    WorkContext m_work_context;
    net::signal_set m_signals;

    std::unique_ptr<SSLContext> m_ssl_context;
    std::function<void()> m_shutdown_hook;

private:
    void InitSignals();

public:
    explicit Context(unsigned int io_threads = std::thread::hardware_concurrency(),
                     unsigned int work_threads = std::thread::hardware_concurrency());

    ~Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    bool EnableSSL();

    /// Runs before Stop() on SIGINT/SIGTERM: the server flushes sessions
    /// (1001 Going Away) and calls Stop() when the drain is done.
    void SetShutdownHook(std::function<void()> hook);

    IOContext& GetIOContext() noexcept
    {
        return m_io_context;
    }

    net::io_context& GetLowerLayerIOContext() noexcept
    {
        return m_io_context.GetIOContext();
    }

    WorkContext& GetWorkContext() noexcept
    {
        return m_work_context;
    }

    SSLContext* GetSSLContext() noexcept
    {
        return m_ssl_context.get();
    }

    const SSLContext* GetSSLContext() const noexcept
    {
        return m_ssl_context.get();
    }

    void Run();
    void Stop();
};

} // namespace run::context