#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <queue>
#include <string>

namespace run::repository
{

class ConnectionPool
{
private:
    ConnectionPool() = default;
    ~ConnectionPool() = default;

    std::queue<std::unique_ptr<pqxx::connection>> m_connections;

    std::mutex m_mutex;
    std::condition_variable m_cvariable;

    std::string m_connection_str;
    size_t m_pool_size = 0;
    bool m_initialized = false;

public:
    static ConnectionPool& Get();

    void Init(const std::string& connection_name, size_t pool_size);

    using ConnectionPtr = std::unique_ptr<pqxx::connection, std::function<void(pqxx::connection*)>>;
    ConnectionPtr GetConnection();
};

} // namespace run::repository