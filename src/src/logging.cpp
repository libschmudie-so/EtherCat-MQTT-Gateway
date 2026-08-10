#include "ecmqtt/logging.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace ecmqtt {

void InitLogging(spdlog::level::level_enum level) {
    auto logger = spdlog::stdout_color_mt("ethercat-mqtt-gateway");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e %^%l%$ %v");
    spdlog::set_level(level);
    spdlog::flush_on(spdlog::level::warn);
}

} // namespace ecmqtt
