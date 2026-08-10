#pragma once

#include <spdlog/spdlog.h>

namespace ecmqtt {

// Configures the default spdlog logger: console sink, timestamped single-line
// pattern, and the given minimum level.
void InitLogging(spdlog::level::level_enum level);

} // namespace ecmqtt
