#pragma once

#include <memory>

#include "config/config.hpp"
#include "context/work_context.hpp"
#include "controllers/http_controller.hpp"
#include "controllers/protobuf_controller.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/iredis_repository.hpp"
#include "repository/iuser_repository.hpp"
#include "service/auth_service.hpp"
#include "service/running/service_stack.hpp"

namespace run::server
{

struct ServerDependencies
{
    std::shared_ptr<repository::IUserRepository> user_repository;
    std::shared_ptr<repository::IRedisRepository> redis_repository;
    std::shared_ptr<repository::IRunHistoryRepository> run_history_repository;
    std::shared_ptr<service::AuthService> auth_service;
    std::shared_ptr<controller::HttpController> http_controller;
    std::shared_ptr<controller::ProtobufController> protobuf_controller;
    std::shared_ptr<service::running::ServiceStack> service_stack;
};

class ServerBootstrap
{
public:
    static ServerDependencies BuildDependencies(context::WorkContext& work_context, const config::ServerConfig& config);

private:
    static bool InitializePostgresPool(const config::PostgresConfig& config);
    static void SeedDevUserIfConfigured(repository::IUserRepository& repo, const config::DevConfig& config);
};

} // namespace run::server
