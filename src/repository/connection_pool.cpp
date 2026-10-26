#include "repository/connection_pool.hpp"
#include "logger/logger.hpp"

namespace run::repository
{

ConnectionPool& ConnectionPool::Get()
{
    static ConnectionPool instance;
    return instance;
}

void ConnectionPool::Init(const std::string& connection_str, size_t pool_size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized)
    {
        return;
    }

    m_connection_str = connection_str;
    m_pool_size = pool_size;

    for (size_t i = 0; i < pool_size; ++i)
    {
        try
        {
            m_connections.push(std::make_unique<pqxx::connection>(m_connection_str));
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to establish PostgreSQL connection: {}", e.what());
            throw;
        }
    }

    m_initialized = true;
    LOG_INFO("PostgreSQL Connection Pool initialized with {} connections", pool_size);
}

ConnectionPool::ConnectionPtr ConnectionPool::GetConnection()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_initialized)
    {
        throw std::runtime_error("ConnectionPool is not initialized");
    }

    m_cvariable.wait(lock, [this]() { return !m_connections.empty(); });

    auto conn = std::move(m_connections.front());
    m_connections.pop();

    return ConnectionPtr(conn.release(),
                         [this](pqxx::connection* p)
                         {
                             std::lock_guard<std::mutex> lock(m_mutex);
                             m_connections.push(std::unique_ptr<pqxx::connection>(p));
                             m_cvariable.notify_one();
                         });
}

} // namespace run::repository