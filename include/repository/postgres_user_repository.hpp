#pragma once

#include "context/work_context.hpp"
#include "repository/iuser_repository.hpp"

namespace run::repository
{

class PostgresUserRepository : public IUserRepository
{
private:
    run::context::WorkContext& m_work_context;

public:
    explicit PostgresUserRepository(run::context::WorkContext& work_context);
    ~PostgresUserRepository() override = default;

    net::awaitable<std::optional<User>> FindById(uint64_t id) override;
    net::awaitable<std::optional<User>> FindByUsername(const std::string& username) override;
    net::awaitable<std::optional<User>> FindByEmail(const std::string& email) override;
    net::awaitable<std::optional<UserStats>> GetUserStats(uint64_t user_id) override;
    net::awaitable<bool> CreateUser(const User& user) override;
};

} // namespace run::repository