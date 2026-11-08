#pragma once

#include "controllers/protobuf_controller.hpp"
#include "service/running/running_services.hpp"

#include <memory>

namespace run::controller::routes
{

void SetupProtobufRoutes(controller::ProtobufController& protobuf_controller,
                         std::shared_ptr<service::running::RunningServices> services);

} // namespace run::controller::routes
