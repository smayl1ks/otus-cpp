#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "common.hpp"
#include "envelope.pb.h"

namespace run::server::session
{

/**
 * @brief Abstract interface representing an active user connection session.
 *
 * Send/SendProtobuf are thread-safe and never block: payloads go into a
 * bounded concurrent channel drained by the session's write loop; on overflow
 * the payload is dropped (and counted). RequestClose flushes queued payloads
 * and finishes the WebSocket with the given close code — 1000 Normal Closure
 * for purposeful closes (run finished, slow/malformed client); 1001 Going
 * Away is reserved for the server shutdown hook. Close is the hard cut.
 */
class UserSession
{
public:
    virtual ~UserSession() = default;

    virtual net::any_io_executor GetExecutor() = 0;
    virtual uint64_t GetId() const noexcept = 0;

    virtual void Send(std::string bytes) = 0;
    virtual void SendProtobuf(const run::proto::Envelope& envelope) = 0;
    virtual void RequestClose(int close_code = 1000) = 0;
    virtual void Close() = 0;

    // viewport subscriptions, res=6 parents
    virtual void UpdateSubscribedZones(const std::vector<uint64_t>& zones) = 0;
    virtual const std::unordered_set<uint64_t>& GetSubscribedZones() const noexcept = 0;

    // live-run cache for radar routing (source of truth is the run service)
    virtual uint64_t GetActiveRunId() const noexcept = 0;
    virtual void SetActiveRunId(uint64_t run_id) noexcept = 0;

    // Confirmed team membership for radar fan-out filtering (source of truth
    // is the user profile; refreshed on every viewport re-subscribe). nullopt
    // = no team: the session only sees public radar
    virtual std::optional<uint64_t> GetTeamId() const noexcept = 0;
    virtual void SetTeamId(std::optional<uint64_t> team_id) noexcept = 0;
};

} // namespace run::server::session
