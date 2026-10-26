#pragma once

// In-memory implementations of the auth repositories. Production Redis /
// PostgreSQL implementations replace them at the composition root without
// touching services.

#include "repository/iredis_repository.hpp"
#include "repository/iuser_repository.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace run::repository
{

namespace net = boost::asio;

class InMemoryUserRepository final : public IUserRepository
{
public:
    /// Dev seeding: e.g. a single user from env before PostgreSQL lands.
    void PutUserForTest(const User& user)
    {
        std::lock_guard lock(m_mutex);
        m_users_by_id[user.id] = user;
        m_users_by_name[user.username] = user.id;
        m_users_by_email[user.email] = user.id;
    }

    net::awaitable<std::optional<User>> FindById(uint64_t id) override
    {
        std::lock_guard lock(m_mutex);
        co_return CopyOf(m_users_by_id, m_users_by_id.find(id));
    }

    net::awaitable<std::optional<User>> FindByUsername(const std::string& username) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_users_by_name.find(username);
        co_return it != m_users_by_name.end() ? CopyOf(m_users_by_id, m_users_by_id.find(it->second))
                                              : std::optional<User>{std::nullopt};
    }

    net::awaitable<std::optional<User>> FindByEmail(const std::string& email) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_users_by_email.find(email);
        co_return it != m_users_by_email.end() ? CopyOf(m_users_by_id, m_users_by_id.find(it->second))
                                               : std::optional<User>{std::nullopt};
    }

    net::awaitable<std::optional<UserStats>> GetUserStats(uint64_t user_id) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_stats.find(user_id);
        if (it == m_stats.end())
        {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    net::awaitable<bool> CreateUser(const User& user) override
    {
        std::lock_guard lock(m_mutex);
        if (m_users_by_name.contains(user.username) || m_users_by_email.contains(user.email))
        {
            co_return false;
        }
        User stored = user;
        if (stored.id == 0)
        {
            stored.id = m_next_id++;
        }
        m_users_by_id[stored.id] = stored;
        m_users_by_name[stored.username] = stored.id;
        m_users_by_email[stored.email] = stored.id;
        co_return true;
    }

private:
    static std::optional<User> CopyOf(const std::map<uint64_t, User>& users,
                                      std::map<uint64_t, User>::const_iterator it)
    {
        return it != users.end() ? std::optional(it->second) : std::nullopt;
    }

    mutable std::mutex m_mutex;
    std::map<uint64_t, User> m_users_by_id;
    std::map<std::string, uint64_t> m_users_by_name;
    std::map<std::string, uint64_t> m_users_by_email;
    std::map<uint64_t, UserStats> m_stats;
    uint64_t m_next_id{1};
};

class InMemoryRedisRepository final : public IRedisRepository
{
public:
    net::awaitable<bool> Set(const std::string& key, const std::string& value, std::chrono::seconds ttl) override
    {
        std::lock_guard lock(m_mutex);
        m_entries[key] = Entry{value, std::chrono::steady_clock::now() + ttl};
        co_return true;
    }

    net::awaitable<std::optional<std::string>> Get(const std::string& key) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_entries.find(key);
        if (it == m_entries.end())
        {
            co_return std::nullopt;
        }
        if (std::chrono::steady_clock::now() > it->second.expires_at)
        {
            m_entries.erase(it);
            co_return std::nullopt;
        }
        co_return it->second.value;
    }

    net::awaitable<bool> Delete(const std::string& key) override
    {
        std::lock_guard lock(m_mutex);
        co_return m_entries.erase(key) > 0;
    }

private:
    struct Entry
    {
        std::string value;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
};

} // namespace run::repository
