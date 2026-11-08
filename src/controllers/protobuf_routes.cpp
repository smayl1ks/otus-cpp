#include "controllers/protobuf_routes.hpp"

#include "controllers/map_controller.hpp"
#include "network/server/envelope_dispatcher.hpp"

namespace run::controller::routes
{

namespace
{

bool ShouldCloseSessionAfter(const run::proto::Envelope& response)
{
    if (response.payload_case() == run::proto::Envelope::PayloadCase::kFinishRunResponse)
    {
        return response.finish_run_response().system_status() == common::SYSTEM_STATUS_OK;
    }
    if (response.payload_case() == run::proto::Envelope::PayloadCase::kCancelRunResponse)
    {
        return response.cancel_run_response().system_status() == common::SYSTEM_STATUS_OK;
    }
    return false;
}

} // namespace

void SetupProtobufRoutes(controller::ProtobufController& protobuf_controller,
                         std::shared_ptr<service::running::RunningServices> services)
{
    const auto map_controller = std::make_shared<MapController>(services);

    protobuf_controller.SetFallbackHandler(
        [map_controller, services](const run::proto::Envelope& envelope,
                                   run::server::session::UserSession& session) -> net::awaitable<void>
        {
            if (envelope.payload_case() == run::proto::Envelope::PayloadCase::kSubscribeViewportRequest)
            {
                co_await map_controller->HandleSubscribeViewport(envelope, session);
                co_return;
            }

            auto response =
                co_await run::server::DispatchAsync(static_cast<int64_t>(session.GetId()), envelope, *services);
            if (response.has_value())
            {
                session.SendProtobuf(*response);

                if (ShouldCloseSessionAfter(*response))
                {
                    session.RequestClose();
                }
            }
        });
}

} // namespace run::controller::routes
