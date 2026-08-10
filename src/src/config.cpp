#include "ecmqtt/config.hpp"

#include <cxxopts.hpp>
#include <iostream>
#include <stdexcept>

namespace ecmqtt {

// IGH's mailbox/SDO transactions (including the automatic ones its own bus
// scan performs) ride on the cyclic exchange, so too slow a cycle rate can
// let their internal timeouts expire -- see RecommendedMinFrequencyHz().
// SOEM's SDO reads are separate blocking calls made during configure(),
// decoupled from cycle rate, so a much lower default is fine there.
#if defined(EC_BACKEND_IGH)
constexpr uint32_t kDefaultFrequencyHz = 100;
constexpr uint32_t kRecommendedMinFrequencyHz = 50;
constexpr uint32_t kMinFrequencyHz = 50;
constexpr uint32_t kMaxFrequencyHz = 5000;
#else
constexpr uint32_t kDefaultFrequencyHz = 10;
constexpr uint32_t kRecommendedMinFrequencyHz = 0;
constexpr uint32_t kMinFrequencyHz = 1;
constexpr uint32_t kMaxFrequencyHz = 1000;
#endif

uint32_t RecommendedMinFrequencyHz() { return kRecommendedMinFrequencyHz; }

std::optional<Config> ParseArgs(int argc, char** argv) {
    cxxopts::Options options("ethercat-mqtt-gateway", "EtherCAT <-> MQTT bridge");

    // clang-format off
    options.add_options()
        ("i,iface", "Network interface for EtherCAT (e.g., eth0).", cxxopts::value<std::string>())
        ("b,broker", "MQTT broker address.", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("p,port", "MQTT broker port.", cxxopts::value<int>()->default_value("1883"))
        ("t,topic", "MQTT root topic.", cxxopts::value<std::string>()->default_value("ethercat"))
        ("e,esi", "ESI directory path.", cxxopts::value<std::string>())
        ("f,frequency", "Cycle frequency in Hz.", cxxopts::value<uint32_t>()->default_value(std::to_string(kDefaultFrequencyHz)))
        ("retain", "Retain process-data messages.", cxxopts::value<bool>()->default_value("false"))
        ("no-output", "Do not publish the output process data.", cxxopts::value<bool>()->default_value("false"))
        ("v,verbose", "Verbose logging (Information).", cxxopts::value<bool>()->default_value("false"))
        ("q,quiet", "Quiet logging (Warning).", cxxopts::value<bool>()->default_value("false"))
        ("debug", "Debug logging.", cxxopts::value<bool>()->default_value("false"))
        ("client-id", "MQTT client ID.", cxxopts::value<std::string>()->default_value("EtherCATMaster"))
        ("use-reported-csa", "Use reported CSA instead of ring CSA for MQTT topics.", cxxopts::value<bool>()->default_value("false"))
        ("realtime", "Request SCHED_FIFO + mlockall for the cycle thread. Validate on your target first: "
                     "on some systems this can starve the kernel's own EtherCAT master thread instead of helping.",
         cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print usage.");
    // clang-format on

    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& ex) {
        std::cerr << "Error parsing options: " << ex.what() << "\n\n" << options.help() << std::endl;
        return std::nullopt;
    }

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return std::nullopt;
    }

    if (!result.count("iface") || result["iface"].as<std::string>().empty())
        throw std::invalid_argument("--iface is required");

    Config cfg;
    cfg.interface = result["iface"].as<std::string>();
    cfg.broker = result["broker"].as<std::string>();
    cfg.port = result["port"].as<int>();
    cfg.topic = result["topic"].as<std::string>();
    if (result.count("esi"))
        cfg.esiDir = result["esi"].as<std::string>();
    cfg.frequencyHz = result["frequency"].as<uint32_t>();
    cfg.retainProcessData = result["retain"].as<bool>();
    cfg.noPublishOutputs = result["no-output"].as<bool>();
    cfg.clientId = result["client-id"].as<std::string>();
    cfg.useReportedCsa = result["use-reported-csa"].as<bool>();
    cfg.realtime = result["realtime"].as<bool>();

    if (cfg.port <= 0 || cfg.port > 65535)
        throw std::invalid_argument("Invalid --port");
    if (cfg.frequencyHz < kMinFrequencyHz || cfg.frequencyHz > kMaxFrequencyHz)
        throw std::invalid_argument("Invalid --frequency (" + std::to_string(kMinFrequencyHz) + ".." +
                                     std::to_string(kMaxFrequencyHz) + " Hz for this backend)");

    // Priority: --debug > --quiet > --verbose > default(Information)
    cfg.logLevel = spdlog::level::info;
    if (result["verbose"].as<bool>()) cfg.logLevel = spdlog::level::info;
    if (result["quiet"].as<bool>()) cfg.logLevel = spdlog::level::warn;
    if (result["debug"].as<bool>()) cfg.logLevel = spdlog::level::debug;

    return cfg;
}

} // namespace ecmqtt
