#pragma once

#include "common.hpp"

#include "repository/iuser_repository.hpp"
#include "service/auth_service.hpp"

#include <nlohmann/json.hpp>

namespace run::controller
{

/// REST plane: auth + read endpoints. Writes (run lifecycle, telemetry) live
/// on the WebSocket contract (proto/envelope.proto), not here.
class HttpController
{
private:
    std::shared_ptr<run::service::AuthService> m_auth_service;
    std::shared_ptr<run::repository::IUserRepository> m_user_repository;

public:
    HttpController(std::shared_ptr<run::service::AuthService> auth_service,
                   std::shared_ptr<run::repository::IUserRepository> user_repository)
        : m_auth_service(std::move(auth_service)), m_user_repository(std::move(user_repository))
    {
    }

    ~HttpController() = default;

    HttpController(const HttpController&) = delete;
    HttpController& operator=(const HttpController&) = delete;

    net::awaitable<http::response<http::string_body>> HandleRequest(http::request<http::string_body> request);

private:
    // POST /api/v1/auth/login
    net::awaitable<http::response<http::string_body>> HandleLogin(http::request<http::string_body> request);
    // POST /api/v1/auth/ws-ticket
    net::awaitable<http::response<http::string_body>> HandleWsTicket(http::request<http::string_body> request);

    // GET /api/v1/users/me | /api/v1/users/{user_id}  (Bearer JWT required)
    net::awaitable<http::response<http::string_body>> HandleProfile(uint64_t user_id, unsigned version);
};

} // namespace run::controller
