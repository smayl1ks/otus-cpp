#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "envelope.pb.h"
#include "logger/logger.hpp"
#include "network/server/user_session.hpp"

namespace run::controller
{

class ProtobufController
{
private:
    using CaseType = run::proto::Envelope::PayloadCase;
    using HandlerFunc =
        std::function<net::awaitable<void>(const run::proto::Envelope&, run::server::session::UserSession&)>;

    std::unordered_map<CaseType, HandlerFunc> m_handlers;
    HandlerFunc m_fallback;

public:
    ProtobufController() = default;
    ~ProtobufController() = default;

    ProtobufController(const ProtobufController&) = delete;
    ProtobufController& operator=(const ProtobufController&) = delete;

    /// Registers a command handler for a specific Protobuf payload case
    void RegisterHandler(CaseType payload_case, HandlerFunc handler)
    {
        m_handlers[payload_case] = std::move(handler);
    }

    /// Handles every case without an explicit handler
    void SetFallbackHandler(HandlerFunc handler)
    {
        m_fallback = std::move(handler);
    }

    /// Dispatches an incoming Envelope to its matching registered handler
    net::awaitable<void> Dispatch(const run::proto::Envelope& envelope,
                                  run::server::session::UserSession& session) const
    {
        const auto it = m_handlers.find(envelope.payload_case());
        if (it != m_handlers.end())
        {
            co_await it->second(envelope, session);
        }
        else if (m_fallback)
        {
            co_await m_fallback(envelope, session);
        }
        else
        {
            LOG_WARN("Unhandled protobuf payload case: {}", static_cast<int>(envelope.payload_case()));
        }
    }
};

} // namespace run::controller
