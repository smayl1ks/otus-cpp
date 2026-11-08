#pragma once

#include "logger/logger.hpp"
#include "service/running/running_services.hpp"
#include "utils/time.hpp"

#include "envelope.pb.h"

#include <boost/asio/awaitable.hpp>
#include <optional>

namespace run::server
{

inline run::proto::Envelope MakeResponseEnvelope(const run::proto::Envelope& request)
{
    run::proto::Envelope response;
    response.set_correlation_id(request.message_id()); // WS replies are unordered
    response.set_timestamp(utils::NowMs());
    response.set_protocol_version(request.protocol_version());
    return response;
}

boost::asio::awaitable<std::optional<run::proto::Envelope>>
DispatchAsync(int64_t user_id, const run::proto::Envelope& request, service::running::RunningServices& services);

} // namespace run::server
