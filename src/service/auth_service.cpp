#include "service/auth_service.hpp"
#include "logger/logger.hpp"
#include "utils/crypto.hpp"

namespace run::service
{

constexpr std::chrono::seconds gTempTolenTTL{30};
constexpr std::chrono::seconds g_JWTAccessTokenTTL{std::chrono::hours(24 * 10)}; // 10 days
constexpr std::string_view g_TempTokenPrefix = "ws_ticket:";

net::awaitable<std::optional<AuthResult>> AuthService::Authenticate(std::string username, std::string password)
{
    auto user = co_await m_user_repository->FindByUsername(username);
    if (!user.has_value())
    {
        co_return std::nullopt;
    }

    bool is_valid = co_await m_work_context.AsyncPost([password = password, hash = user->password_hash]() -> bool
                                                      { return run::utils::crypto::VerifyPassword(password, hash); });

    if (!is_valid)
    {
        co_return std::nullopt;
    }

    std::string jwt_secret = config::ServerConfig::Get().GetJwtSecret();

    std::string access_token = run::utils::crypto::GenerateAccessToken(user->id, jwt_secret, g_JWTAccessTokenTTL);

    auto ws_ticket_opt = co_await CreateWsTicket(user->id);

    if (!ws_ticket_opt.has_value())
    {
        co_return std::nullopt;
    }

    AuthResult result;
    result.access_token = std::move(access_token);
    result.ws_ticket = std::move(*ws_ticket_opt);
    result.expires_in = static_cast<uint32_t>(g_JWTAccessTokenTTL.count());

    co_return result;
}

net::awaitable<std::optional<std::string>> AuthService::CreateWsTicket(uint64_t user_id)
{
    std::string temp_token = run::utils::crypto::GenerateRefreshToken();
    std::string key = std::string(g_TempTokenPrefix) + temp_token;

    bool saved = co_await m_redis_repository->Set(key, std::to_string(user_id), gTempTolenTTL);
    if (!saved)
    {
        LOG_ERROR("Failed to store temporary WS ticket in Redis for user {}", user_id);
        co_return std::nullopt;
    }

    co_return temp_token;
}

net::awaitable<std::optional<std::string>> AuthService::CreateWsTicketFromJwt(std::string_view bearer_token)
{
    if (bearer_token.empty())
    {
        co_return std::nullopt;
    }

    std::string_view token = bearer_token;
    if (token.starts_with("Bearer ") || token.starts_with("bearer "))
    {
        token = token.substr(7);
    }

    std::string jwt_secret = config::ServerConfig::Get().GetJwtSecret();
    auto user_id_opt = run::utils::crypto::VerifyAccessToken(token, jwt_secret);
    if (!user_id_opt.has_value())
    {
        co_return std::nullopt;
    }

    co_return co_await CreateWsTicket(*user_id_opt);
}

net::awaitable<std::optional<uint64_t>> AuthService::ValidateAccessToken(std::string_view bearer_token)
{
    if (bearer_token.empty())
    {
        co_return std::nullopt;
    }

    std::string_view token = bearer_token;
    if (token.starts_with("Bearer ") || token.starts_with("bearer "))
    {
        token = token.substr(7);
    }

    const std::string jwt_secret = config::ServerConfig::Get().GetJwtSecret();
    co_return run::utils::crypto::VerifyAccessToken(token, jwt_secret);
}

net::awaitable<std::optional<uint64_t>> AuthService::ValidateTempToken(const std::string& token)
{
    if (token.empty())
    {
        co_return std::nullopt;
    }

    std::string key = std::string(g_TempTokenPrefix) + token;
    auto val_opt = co_await m_redis_repository->Get(key);
    if (!val_opt.has_value() || val_opt->empty())
    {
        co_return std::nullopt;
    }

    co_await m_redis_repository->Delete(key);

    try
    {
        uint64_t user_id = std::stoull(*val_opt);
        co_return user_id;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to parse user_id '{}' from Redis token key {}: {}", *val_opt, key, e.what());
        co_return std::nullopt;
    }
}

} // namespace run::service