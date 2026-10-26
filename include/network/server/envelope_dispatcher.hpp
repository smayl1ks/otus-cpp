#pragma once

// WS entry point: maps Envelope oneof payloads to service calls and back.
// The ONLY place aware of the payload dispatch table; services speak domain
// protos (session::/telemetry::) and never see the Envelope.

#include "logger/logger.hpp"
#include "service/running/service_stack.hpp"

#include "envelope.pb.h"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <optional>

namespace run::server
{

inline int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline run::proto::Envelope MakeResponseEnvelope(const run::proto::Envelope& request)
{
    run::proto::Envelope response;
    response.set_correlation_id(request.message_id()); // WS replies are unordered
    response.set_timestamp(NowMs());
    response.set_protocol_version(request.protocol_version());
    return response;
}

/// user_id comes from the authenticated connection context (WS ticket), never
/// from the payload. nullopt = no request payload / unknown message type.
boost::asio::awaitable<std::optional<run::proto::Envelope>>
DispatchAsync(int64_t user_id, const run::proto::Envelope& request, service::running::ServiceStack& services);

} // namespace run::server
