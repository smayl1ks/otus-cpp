#pragma once

#include <memory>
#include <thread>

#include "common.hpp"

#include "config/config.hpp"
#include "logger/logger.hpp"

#include "context/context.hpp"

#include "network/server/http_session.hpp"
#include "network/server/session_manager.hpp"

#include "controllers/http_controller.hpp"
#include "controllers/protobuf_controller.hpp"

#include "repository/in_memory_auth_stores.hpp"
#include "repository/iredis_repository.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/iuser_repository.hpp"

#include "service/auth_service.hpp"
#include "service/running/service_stack.hpp"

namespace run::server
{

/// Composition root: wires listeners, auth, session registry and the run
/// service stack. Storage implementations (in-memory stubs today, PostgreSQL /
/// Redis tomorrow) are chosen here and nowhere else.
class Server
{
private:
    using SessionPtr = std::shared_ptr<session::SessionManager>;

public:
    explicit Server(unsigned int io_threads = std::thread::hardware_concurrency(),
                    unsigned int work_threads = std::thread::hardware_concurrency());

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Blocking: runs the io_context until Stop().
    void Run();

    /// Graceful: 1001 Going Away to every session, short drain, then stop.
    void Stop();

    session::SessionManager& GetSessionManager() noexcept
    {
        return *m_session_manager;
    }

private:
    net::awaitable<void> ListenAsync(unsigned short port, bool use_tls);
    void SetupProtobufRoutes();

    run::context::Context m_context;

    SessionPtr m_session_manager;

    // Persistence: PostgreSQL repositories when the pool connects, in-memory
    // stubs otherwise (dev without docker). Chosen once, in the ctor.
    std::shared_ptr<run::repository::IUserRepository> m_user_repository;
    std::shared_ptr<run::repository::IRedisRepository> m_redis_repository;
    std::shared_ptr<run::repository::IRunHistoryRepository> m_run_history;
    std::shared_ptr<run::service::AuthService> m_auth_service;

    // controllers
    std::shared_ptr<controller::HttpController> m_http_controller;
    std::shared_ptr<controller::ProtobufController> m_protobuf_controller;

    // run domain
    std::shared_ptr<run::service::running::ServiceStack> m_service_stack;
};

} // namespace run::server
