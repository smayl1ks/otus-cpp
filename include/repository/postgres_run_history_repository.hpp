#pragma once

#include "context/work_context.hpp"
#include "repository/i_run_history_repository.hpp"

namespace run::repository
{

class PostgresRunHistoryRepository final : public IRunHistoryRepository
{
public:
    explicit PostgresRunHistoryRepository(run::context::WorkContext& work_context);
    ~PostgresRunHistoryRepository() override = default;

    boost::asio::awaitable<std::optional<int64_t>> CreateRunAsync(const NewRunRecord& record) override;

    boost::asio::awaitable<FinalizeStatus>
    FinalizeRunAsync(const StoredRunSummary& summary, std::span<const gameplay::workout_tracking::RunPoint> track,
                     std::vector<HexCaptureRecord>* captured_hexes_out = nullptr) override;

    boost::asio::awaitable<std::optional<StoredRunSummary>>
    GetFinalizedRunAsync(const std::string& session_id, int64_t user_id) override;

private:
    run::context::WorkContext& m_work_context;
};

} // namespace run::repository
