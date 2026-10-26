#include "context/work_context.hpp"

namespace run::context
{

WorkContext::WorkContext(unsigned int threads) : m_thread_pool(threads > 0 ? threads : 1)
{
}

WorkContext::~WorkContext()
{
    Stop();
}

void WorkContext::Stop()
{
    m_thread_pool.stop();
    m_thread_pool.join();
}

} // namespace run::context