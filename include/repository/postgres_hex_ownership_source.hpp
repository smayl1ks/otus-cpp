#pragma once

#include "context/work_context.hpp"
#include "repository/i_hex_ownership_source.hpp"

namespace run::repository
{

class PostgresHexOwnershipSource final : public IHexOwnershipSource
{
public:
    explicit PostgresHexOwnershipSource(run::context::WorkContext& work_context) : m_work_context(work_context)
    {
    }

    boost::asio::awaitable<std::vector<HexOwnershipRecord>>
    GetByHexIdsAsync(std::span<const uint64_t> h3_indexes) override;

    boost::asio::awaitable<std::vector<HexOwnershipRecord>> GetAllCapturedAsync() override;

    boost::asio::awaitable<std::optional<HexDetailsRecord>> GetHexDetailsAsync(uint64_t h3_index) override;

private:
    run::context::WorkContext& m_work_context;
};

} // namespace run::repository
