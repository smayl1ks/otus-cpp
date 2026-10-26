#include "context/context.hpp"

namespace run::context
{

Context::Context(unsigned int io_threads, unsigned int work_threads)
    : m_io_context(io_threads), m_work_context(work_threads), m_signals(m_io_context.GetIOContext(), SIGINT, SIGTERM)
{
    InitSignals();
}

bool Context::EnableSSL()
{
    m_ssl_context = std::make_unique<SSLContext>();
    return m_ssl_context != nullptr;
}

void Context::InitSignals()
{
    m_signals.async_wait(
        [this](const sys::error_code&, int signal_number)
        {
            LOG_INFO("Signal received: ({}). Initiating graceful shutdown ...", signal_number);
            if (m_shutdown_hook)
            {
                // Server hook: flush sessions (1001 Going Away), then Stop().
                m_shutdown_hook();
            }
            else
            {
                Stop();
            }
        });
}

void Context::SetShutdownHook(std::function<void()> hook)
{
    m_shutdown_hook = std::move(hook);
}

void Context::Run()
{
    m_io_context.Run();
}

void Context::Stop()
{
    m_work_context.Stop();
    m_io_context.Stop();
}

} // namespace run::context