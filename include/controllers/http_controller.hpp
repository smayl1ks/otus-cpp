#pragma once

#include "common.hpp"

#include "repository/i_hex_ownership_source.hpp"
#include "repository/iuser_repository.hpp"
#include "service/auth_service.hpp"

#include <nlohmann/json.hpp>

namespace run::controller
{

class HttpController
{
private:
    std::shared_ptr<run::service::AuthService> m_auth_service;
    std::shared_ptr<run::repository::IUserRepository> m_user_repository;
    std::shared_ptr<run::repository::IHexOwnershipSource> m_hex_ownership_source;

public:
    HttpController(std::shared_ptr<run::service::AuthService> auth_service,
                   std::shared_ptr<run::repository::IUserRepository> user_repository,
                   std::shared_ptr<run::repository::IHexOwnershipSource> hex_ownership_source)
        : m_auth_service(std::move(auth_service)), m_user_repository(std::move(user_repository)),
          m_hex_ownership_source(std::move(hex_ownership_source))
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

    // GET /api/v1/users/me | /api/v1/users/{user_id} (Bearer JWT required)
    net::awaitable<http::response<http::string_body>> HandleProfile(uint64_t user_id, unsigned version);

    // GET /api/v1/hexagons/{h3_index}/details (Bearer JWT required)
    net::awaitable<http::response<http::string_body>> HandleHexDetails(uint64_t h3_index,
                                                                       http::request<http::string_body>& request);
};

} // namespace run::controller
