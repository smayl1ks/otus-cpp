#pragma once

#include "gameplay/territory_conquest/zone_channel_helper.hpp"
#include "logger/logger.hpp"
#include "service/running/radar_publisher.hpp"
#include "service/running/run_finalizer.hpp"

#include "envelope.pb.h"
#include "gamemap.pb.h"

#include <memory>
#include <string>

namespace run::network::mappers
{

inline gamemap::HexagonCaptureEvent ToCaptureEventProto(const service::running::CaptureNotice& notice)
{
    gamemap::HexagonCaptureEvent event;
    event.set_h3_index(notice.h3_index);
    event.set_new_owner_id(static_cast<uint64_t>(notice.new_owner_id));
    event.set_new_owner_name(notice.new_owner_name);
    event.set_new_owner_color_hex(notice.new_owner_color_hex);
    if (notice.prev_owner_id.has_value())
    {
        event.set_prev_owner_id(static_cast<uint64_t>(*notice.prev_owner_id));
    }
    event.set_score_at_capture(notice.score_at_capture);
    event.set_timestamp(notice.timestamp_ms);
    return event;
}

class CaptureEventPublisher final : public service::running::ICaptureEventPublisher
{
public:
    explicit CaptureEventPublisher(std::shared_ptr<service::running::IPubSubPublisher> transport)
        : m_transport(std::move(transport))
    {
    }

    net::awaitable<void> PublishAsync(const service::running::CaptureNotice& notice) override
    {
        if (!m_transport)
        {
            co_return;
        }

        run::proto::Envelope envelope;
        envelope.set_timestamp(notice.timestamp_ms);
        envelope.set_protocol_version(1);
        *envelope.mutable_hexagon_capture_event() = ToCaptureEventProto(notice);

        std::string payload;
        if (!envelope.SerializeToString(&payload))
        {
            LOG_WARN("[capture-fanout] failed to serialize capture event for hex {:x}", notice.h3_index);
            co_return;
        }

        if (!(co_await m_transport->PublishAsync(
                gameplay::territory_conquest::ZoneChannelHelper::FormatChannel(notice.h3_index), payload)))
        {
            LOG_WARN("[capture-fanout] publish failed for hex {:x}", notice.h3_index);
        }
    }

private:
    std::shared_ptr<service::running::IPubSubPublisher> m_transport;
};

} // namespace run::network::mappers
