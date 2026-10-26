#pragma once

#include <boost/asio.hpp>

#include <optional>
#include <string>

namespace net = boost::asio;

namespace run::repository
{

struct User
{
    uint64_t id{0};
    std::optional<uint64_t> team_id{std::nullopt};
    std::string username;
    std::string email;
    std::string password_hash;
    std::string player_color_hex{"#000000"};
    std::string created_at;
};

struct UserStats
{
    uint64_t user_id{0};
    double total_distance_meters{0.0};
    int64_t total_duration_seconds{0};
    int32_t total_runs{0};
    int32_t total_game_points{0};
    int32_t current_held_hexagons{0};
    std::string updated_at;
};

class IUserRepository
{
public:
    virtual ~IUserRepository() = default;

    virtual net::awaitable<std::optional<User>> FindById(uint64_t id) = 0;
    virtual net::awaitable<std::optional<User>> FindByUsername(const std::string& username) = 0;
    virtual net::awaitable<std::optional<User>> FindByEmail(const std::string& email) = 0;
    virtual net::awaitable<std::optional<UserStats>> GetUserStats(uint64_t user_id) = 0;
    virtual net::awaitable<bool> CreateUser(const User& user) = 0;
};

} // namespace run::repository