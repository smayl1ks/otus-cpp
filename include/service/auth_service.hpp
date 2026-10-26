#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common.hpp"
#include "context/work_context.hpp"
#include "repository/iredis_repository.hpp"
#include "repository/iuser_repository.hpp"

#include "config/config.hpp"

namespace run::service
{

struct AuthResult
{
    std::string access_token;
    std::string ws_ticket;
    uint32_t expires_in{0};
};

class AuthService
{
private:
    std::shared_ptr<run::repository::IUserRepository> m_user_repository;
    std::shared_ptr<run::repository::IRedisRepository> m_redis_repository;
    run::context::WorkContext& m_work_context;

public:
    AuthService(std::shared_ptr<run::repository::IUserRepository> user_repository,
                std::shared_ptr<run::repository::IRedisRepository> redis_repository, run::context::WorkContext& work_context)
        : m_user_repository(std::move(user_repository)), m_redis_repository(std::move(redis_repository)),
          m_work_context(work_context)
    {
    }

    net::awaitable<std::optional<AuthResult>> Authenticate(std::string username, std::string password);
    net::awaitable<std::optional<std::string>> CreateWsTicket(uint64_t user_id);
    net::awaitable<std::optional<std::string>> CreateWsTicketFromJwt(std::string_view bearer_token);
    net::awaitable<std::optional<uint64_t>> ValidateTempToken(const std::string& token);

    /// Verifies an "Authorization: Bearer <JWT>" header value, returns the user id.
    /// Accepts the raw header value (with or without the "Bearer " prefix).
    net::awaitable<std::optional<uint64_t>> ValidateAccessToken(std::string_view bearer_token);
};

} // namespace run::service