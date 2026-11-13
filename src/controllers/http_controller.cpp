#include "controllers/http_controller.hpp"
#include "monitoring/metrics_collector.hpp"

#include "logger/logger.hpp"

#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>

namespace run::controller
{

namespace
{

constexpr auto g_ServerName = "Run Server";

using Response = http::response<http::string_body>;

std::string_view PathOnly(std::string_view target)
{
    const auto query = target.find('?');
    return query == std::string_view::npos ? target : target.substr(0, query);
}

Response MakeResponse(const http::request<http::string_body>& request, http::status status, const nlohmann::json& body)
{
    Response response{status, request.version()};
    response.set(http::field::server, g_ServerName);
    response.set(http::field::content_type, "application/json");
    response.keep_alive(request.keep_alive());
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

Response MakeError(http::status status, std::string_view message, unsigned version = 11)
{
    Response response{status, version};
    response.set(http::field::server, g_ServerName);
    response.set(http::field::content_type, "application/json");
    response.body() = nlohmann::json{{"status", "error"}, {"message", message}}.dump();
    response.prepare_payload();
    return response;
}

Response MakeSuccess(const http::request<http::string_body>& request, const nlohmann::json& body)
{
    nlohmann::json payload = {{"status", "success"}};
    payload.update(body);
    return MakeResponse(request, http::status::ok, payload);
}

std::optional<uint64_t> ParseUint64(std::string_view segment)
{
    uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), value);
    return ec == std::errc{} && ptr == segment.data() + segment.size() ? std::optional(value) : std::nullopt;
}

} // namespace

net::awaitable<http::response<http::string_body>> HttpController::HandleLogin(http::request<http::string_body> request)
{
    nlohmann::json body_json;
    try
    {
        body_json = nlohmann::json::parse(request.body());
    }
    catch (const std::exception&)
    {
        co_return MakeError(http::status::bad_request, "Invalid request body", request.version());
    }

    const auto auth =
        co_await m_auth_service->Authenticate(body_json.value("login", ""), body_json.value("password", ""));
    if (!auth.has_value())
    {
        co_return MakeError(http::status::unauthorized, "Invalid credentials", request.version());
    }

    co_return MakeSuccess(
        request,
        {{"access_token", auth->access_token}, {"ws_ticket", auth->ws_ticket}, {"expires_in", auth->expires_in}});
}

net::awaitable<http::response<http::string_body>>
HttpController::HandleWsTicket(http::request<http::string_body> request)
{
    std::string token;
    if (const auto it = request.find(http::field::authorization); it != request.end())
    {
        token = it->value();
    }
    else if (!request.body().empty())
    {
        try
        {
            token = nlohmann::json::parse(request.body()).value("access_token", "");
        }
        catch (const std::exception&)
        {
            // fall through: empty token -> 401
        }
    }

    const auto ticket = co_await m_auth_service->CreateWsTicketFromJwt(token);
    if (!ticket.has_value())
    {
        co_return MakeError(http::status::unauthorized, "Invalid or expired access token", request.version());
    }

    co_return MakeSuccess(request, {{"ws_ticket", *ticket}});
}

net::awaitable<http::response<http::string_body>> HttpController::HandleProfile(uint64_t user_id, unsigned version)
{
    const auto user = co_await m_user_repository->FindById(user_id);
    if (!user.has_value())
    {
        co_return MakeError(http::status::not_found, "User not found", version);
    }

    nlohmann::json profile = {{"id", user->id},
                              {"username", user->username},
                              {"email", user->email},
                              {"player_color_hex", user->player_color_hex},
                              {"created_at", user->created_at}};
    if (user->team_id.has_value())
    {
        profile["team_id"] = *user->team_id;
    }

    if (const auto stats = co_await m_user_repository->GetUserStats(user_id); stats.has_value())
    {
        profile["stats"] = {{"total_distance_meters", stats->total_distance_meters},
                            {"total_duration_seconds", stats->total_duration_seconds},
                            {"total_runs", stats->total_runs},
                            {"total_points", stats->total_game_points},
                            {"current_held_hexagons", stats->current_held_hexagons}};
    }

    Response response{http::status::ok, version};
    response.set(http::field::server, g_ServerName);
    response.set(http::field::content_type, "application/json");
    response.body() = nlohmann::json{{"status", "success"}, {"profile", profile}}.dump();
    response.prepare_payload();
    co_return response;
}

net::awaitable<http::response<http::string_body>>
HttpController::HandleHexDetails(uint64_t h3_index, http::request<http::string_body>& request)
{
    const auto details = co_await m_hex_ownership_source->GetHexDetailsAsync(h3_index);
    if (!details.has_value())
    {
        co_return MakeError(http::status::not_found, "Hexagon not found", request.version());
    }

    nlohmann::json runners = nlohmann::json::array();
    for (const auto& runner : details->top_runners)
    {
        runners.push_back({{"user_id", runner.user_id},
                           {"username", runner.username},
                           {"player_color_hex", runner.player_color_hex},
                           {"game_points", runner.game_points}});
    }

    nlohmann::json body = {{"h3_index", details->h3_index}, {"top_runners", std::move(runners)}};
    if (details->owner_user_id.has_value())
    {
        body["owner_user_id"] = *details->owner_user_id;
        body["owner_username"] = details->owner_username;
        body["owner_color_hex"] = details->owner_color_hex;
        body["owner_score"] = details->owner_score;
    }
    else
    {
        body["owner_user_id"] = nullptr;
        body["owner_username"] = nullptr;
    }

    co_return MakeSuccess(request, std::move(body));
}

net::awaitable<http::response<http::string_body>>
HttpController::HandleRequest(http::request<http::string_body> request)
{
    const std::string_view path = PathOnly(request.target());

    // --- Public endpoints ---
    if (request.method() == http::verb::post && path == "/api/v1/auth/login")
    {
        const auto response = co_await HandleLogin(std::move(request));

        monitoring::MetricsCollector::Instance().IncrementHttpRequests("POST", "/api/v1/auth/login",
                                                                       static_cast<int>(response.result_int()));
        co_return response;
    }
    if (request.method() == http::verb::post && path == "/api/v1/auth/ws-ticket")
    {
        co_return co_await HandleWsTicket(std::move(request));
    }

    if (request.method() == http::verb::get && path == "/api/v1/health")
    {
        monitoring::MetricsCollector::Instance().IncrementHttpRequests("GET", "/api/v1/health", 200);
        co_return MakeResponse(request, http::status::ok, {{"status", "ok"}});
    }

    // --- Protected endpoints ----
    std::optional<uint64_t> caller_id;
    if (const auto it = request.find(http::field::authorization); it != request.end())
    {
        caller_id = co_await m_auth_service->ValidateAccessToken(it->value());
    }
    if (!caller_id.has_value())
    {
        co_return MakeError(http::status::unauthorized, "Missing or invalid access token", request.version());
    }

    // GET /api/v1/users/me
    if (request.method() == http::verb::get && path == "/api/v1/users/me")
    {
        co_return co_await HandleProfile(*caller_id, request.version());
    }

    // GET /api/v1/users/{user_id}
    constexpr std::string_view kUsersPrefix = "/api/v1/users/";
    if (request.method() == http::verb::get && path.starts_with(kUsersPrefix))
    {
        if (const auto user_id = ParseUint64(path.substr(kUsersPrefix.size())); user_id.has_value())
        {
            co_return co_await HandleProfile(*user_id, request.version());
        }
        co_return MakeError(http::status::bad_request, "Invalid user id", request.version());
    }

    // GET /api/v1/hexagons/{h3_index}/details
    constexpr std::string_view kHexPrefix = "/api/v1/hexagons/";
    if (request.method() == http::verb::get && path.starts_with(kHexPrefix) && path.ends_with("/details"))
    {
        const auto hex_segment = path.substr(kHexPrefix.size(), path.size() - kHexPrefix.size() - strlen("/details"));
        if (const auto hex = ParseUint64(hex_segment); hex.has_value())
        {
            co_return co_await HandleHexDetails(*hex, request);
        }
        co_return MakeError(http::status::bad_request, "Invalid h3 index", request.version());
    }

    // GET /api/v1/runs
    if (request.method() == http::verb::get && path == "/api/v1/runs")
    {
        // TODO
        co_return MakeResponse(request, http::status::ok,
                               {{"status", "success"},
                                {"user_id", *caller_id},
                                {"runs", nlohmann::json::array()},
                                {"note", "stub: run history REST is not implemented yet; live data is on WS"}});
    }

    co_return MakeError(http::status::not_found, "Resource not found", request.version());
}

} // namespace run::controller
