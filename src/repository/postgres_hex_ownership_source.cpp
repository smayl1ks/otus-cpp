#include "repository/postgres_hex_ownership_source.hpp"

#include "logger/logger.hpp"
#include "repository/connection_pool.hpp"

#include <fmt/format.h>
#include <pqxx/pqxx>

#include <memory>

namespace run::repository
{

namespace
{

// H3 uint64 indexes are stored in BIGINT columns
constexpr int64_t ToBigInt(uint64_t h3_index)
{
    return static_cast<int64_t>(h3_index);
}

} // namespace

boost::asio::awaitable<std::vector<HexOwnershipRecord>>
PostgresHexOwnershipSource::GetByHexIdsAsync(std::span<const uint64_t> h3_indexes)
{
    if (h3_indexes.empty())
    {
        co_return std::vector<HexOwnershipRecord>{};
    }

    auto ids = std::make_shared<std::vector<int64_t>>();
    ids->reserve(h3_indexes.size());
    for (const uint64_t h3_index : h3_indexes)
    {
        ids->push_back(ToBigInt(h3_index));
    }
    auto result = std::make_shared<std::vector<HexOwnershipRecord>>();

    const bool ok = co_await m_work_context.AsyncPost(
        [ids, result]() -> bool
        {
            try
            {
                auto connection = ConnectionPool::Get().GetConnection();
                pqxx::work txn(*connection);

                pqxx::params query_params;
                query_params.append(*ids);
                const auto rows = txn.exec("SELECT h.h3_index, h.owner_user_id, COALESCE(u.username, ''), "
                                           "       COALESCE(u.player_color_hex, '#000000'), h.top_score "
                                           "FROM hexagons h LEFT JOIN users u ON u.id = h.owner_user_id "
                                           "WHERE h.owner_user_id IS NOT NULL AND h.h3_index = ANY($1::bigint[]) "
                                           "ORDER BY h.h3_index",
                                           query_params);
                result->reserve(rows.size());
                for (const auto& row : rows)
                {
                    result->push_back(HexOwnershipRecord{
                        static_cast<uint64_t>(row[0].as<int64_t>()), static_cast<uint64_t>(row[1].as<int64_t>()),
                        row[2].as<std::string>(), row[3].as<std::string>(), row[4].as<int32_t>()});
                }
                txn.commit();
                return true;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Hex ownership lookup of {} hex(es) failed: {}", ids->size(), e.what());
                return false;
            }
        });

    if (!ok)
    {
        co_return std::vector<HexOwnershipRecord>{};
    }
    co_return std::move(*result);
}

} // namespace run::repository
