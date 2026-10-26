#pragma once

#include "context/work_context.hpp"
#include "repository/i_run_history_repository.hpp"

namespace run::repository
{

// PostgreSQL plane of the run domain (schema: docker/initdb/01_init_runs.sql,
// 02_init_hexagons.sql). CreateRunAsync is called at START (runs row,
// status='active'), FinalizeRunAsync is the single closing transaction from
// RunFinalizer: runs row + run_points batch + hexagon upserts +
// hexagon_history + user_stats.
//
// Blocking pqxx work runs on the WorkContext thread pool; connections come
// from ConnectionPool.
class PostgresRunHistoryRepository final : public IRunHistoryRepository
{
public:
    explicit PostgresRunHistoryRepository(run::context::WorkContext& work_context);
    ~PostgresRunHistoryRepository() override = default;

    boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) override;

    boost::asio::awaitable<bool> FinalizeRunAsync(const StoredRunSummary& summary,
                                                  std::span<const gameplay::workout_tracking::RunPoint> track) override;

private:
    run::context::WorkContext& m_work_context;
};

} // namespace run::repository
