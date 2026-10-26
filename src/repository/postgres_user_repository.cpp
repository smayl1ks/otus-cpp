#include "repository/postgres_user_repository.hpp"
#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"

namespace run::repository
{

namespace
{

User ParseUserRow(const pqxx::row& row)
{
    User user;
    user.id = static_cast<uint64_t>(row["id"].as<int64_t>());
    if (!row["team_id"].is_null())
    {
        user.team_id = static_cast<uint64_t>(row["team_id"].as<int64_t>());
    }
    user.username = row["username"].as<std::string>();
    user.email = row["email"].as<std::string>();
    user.password_hash = row["password_hash"].as<std::string>();
    user.player_color_hex = row["player_color_hex"].as<std::string>();
    user.created_at = row["created_at"].as<std::string>();
    return user;
}

} // namespace

PostgresUserRepository::PostgresUserRepository(context::WorkContext& work_context) : m_work_context(work_context)
{
}

net::awaitable<std::optional<User>> PostgresUserRepository::FindById(uint64_t id)
{
    co_return co_await m_work_context.AsyncPost(
        [id]() -> std::optional<User>
        {
            try
            {
                auto connection_ptr = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection_ptr);

                auto result =
                    txn.exec("SELECT id, team_id, username, email, password_hash, player_color_hex, created_at::text "
                             "FROM users WHERE id = $1",
                             pqxx::params{static_cast<int64_t>(id)});

                if (result.empty())
                {
                    return std::nullopt;
                }

                return ParseUserRow(result[0]);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to find user by id {}: {}", id, e.what());
                return std::nullopt;
            }
        });
}

net::awaitable<std::optional<User>> PostgresUserRepository::FindByUsername(const std::string& username)
{
    co_return co_await m_work_context.AsyncPost(
        [username]() -> std::optional<User>
        {
            try
            {
                auto connection_ptr = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection_ptr);

                auto result =
                    txn.exec("SELECT id, team_id, username, email, password_hash, player_color_hex, created_at::text "
                             "FROM users WHERE username = $1",
                             pqxx::params{username});

                if (result.empty())
                {
                    return std::nullopt;
                }

                return ParseUserRow(result[0]);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to find user by username '{}': {}", username, e.what());
                return std::nullopt;
            }
        });
}

net::awaitable<std::optional<User>> PostgresUserRepository::FindByEmail(const std::string& email)
{
    co_return co_await m_work_context.AsyncPost(
        [email]() -> std::optional<User>
        {
            try
            {
                auto connection_ptr = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection_ptr);

                auto result =
                    txn.exec("SELECT id, team_id, username, email, password_hash, player_color_hex, created_at::text "
                             "FROM users WHERE email = $1",
                             pqxx::params{email});

                if (result.empty())
                {
                    return std::nullopt;
                }

                return ParseUserRow(result[0]);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to find user by email '{}': {}", email, e.what());
                return std::nullopt;
            }
        });
}

net::awaitable<std::optional<UserStats>> PostgresUserRepository::GetUserStats(uint64_t user_id)
{
    co_return co_await m_work_context.AsyncPost(
        [user_id]() -> std::optional<UserStats>
        {
            try
            {
                auto connection_ptr = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection_ptr);

                auto result = txn.exec("SELECT user_id, total_distance_meters, total_duration_seconds, "
                                       "       total_runs, total_game_points, current_held_hexagons, updated_at::text "
                                       "FROM user_stats WHERE user_id = $1",
                                       pqxx::params{static_cast<int64_t>(user_id)});

                if (result.empty())
                {
                    return std::nullopt;
                }

                const auto& row = result[0];
                UserStats stats;
                stats.user_id = static_cast<uint64_t>(row["user_id"].as<int64_t>());
                stats.total_distance_meters = row["total_distance_meters"].as<double>();
                stats.total_duration_seconds = row["total_duration_seconds"].as<int64_t>();
                stats.total_runs = row["total_runs"].as<int32_t>();
                stats.total_game_points = row["total_game_points"].as<int32_t>();
                stats.current_held_hexagons = row["current_held_hexagons"].as<int32_t>();
                stats.updated_at = row["updated_at"].as<std::string>();

                return stats;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to get user stats for user_id {}: {}", user_id, e.what());
                return std::nullopt;
            }
        });
}

net::awaitable<bool> PostgresUserRepository::CreateUser(const User& user)
{
    co_return co_await m_work_context.AsyncPost(
        [user]() -> bool
        {
            try
            {
                auto connection_ptr = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection_ptr);

                pqxx::params p;
                if (user.team_id.has_value())
                {
                    p.append(static_cast<int64_t>(*user.team_id));
                }
                else
                {
                    p.append(nullptr);
                }
                p.append(user.username);
                p.append(user.email);
                p.append(user.password_hash);
                p.append(user.player_color_hex.empty() ? "#000000" : user.player_color_hex);

                txn.exec("INSERT INTO users (team_id, username, email, password_hash, player_color_hex) "
                         "VALUES ($1, $2, $3, $4, $5)",
                         p);

                txn.commit();
                return true;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Failed to create user '{}' (email: '{}'): {}", user.username, user.email, e.what());
                return false;
            }
        });
}

} // namespace run::repository