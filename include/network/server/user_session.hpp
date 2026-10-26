#pragma once

#include <cstdint>
#include <memory>
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
 * and finishes the WebSocket with 1001 Going Away; Close is the hard cut.
 */
class UserSession
{
public:
    virtual ~UserSession() = default;

    virtual net::any_io_executor GetExecutor() = 0;
    virtual uint64_t GetId() const noexcept = 0;

    virtual void Send(std::string bytes) = 0;
    virtual void SendProtobuf(const run::proto::Envelope& envelope) = 0;
    virtual void RequestClose() = 0;
    virtual void Close() = 0;

    // viewport subscriptions, res=6 parents
    virtual void UpdateSubscribedZones(const std::vector<uint64_t>& zones) = 0;
    virtual const std::unordered_set<uint64_t>& GetSubscribedZones() const noexcept = 0;

    // live-run cache for radar routing (source of truth is the run service)
    virtual uint64_t GetActiveRunId() const noexcept = 0;
    virtual void SetActiveRunId(uint64_t run_id) noexcept = 0;
};

} // namespace run::server::session
