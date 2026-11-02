#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <memory>

#include "config/config.hpp"
#include "context/work_context.hpp"
#include "controllers/http_controller.hpp"
#include "controllers/protobuf_controller.hpp"
#include "repository/i_run_history_repository.hpp"
#include "repository/iredis_repository.hpp"
#include "repository/iuser_repository.hpp"
#include "service/auth_service.hpp"
#include "service/running/running_services.hpp"

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
    std::shared_ptr<service::running::RunningServices> running_services;
};

class ServerBootstrap
{
public:
    /// Builds the whole dependency graph. Throws when PostgreSQL is
    /// unreachable: a node that cannot persist must not serve traffic — the
    /// old silent fallback to in-memory stores rejected every login and kept
    /// per-node isolated state until the next restart (audit S7). With
    /// `restart: always` the container retries until the database is up.
    static ServerDependencies BuildDependencies(context::WorkContext& work_context,
                                                boost::asio::any_io_executor io_executor,
                                                const config::ServerConfig& config);
};

} // namespace run::server
