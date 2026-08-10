#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <spdlog/common.h>

namespace ecmqtt {

struct Config {
    std::string interface;
    std::string broker = "127.0.0.1";
    int port = 1883;
    std::string topic = "ethercat";
    std::optional<std::string> esiDir;
    uint32_t frequencyHz = 10;
    bool retainProcessData = false;
    bool noPublishOutputs = false;
    spdlog::level::level_enum logLevel = spdlog::level::info;
    std::string clientId = "EtherCATMaster";
    bool useReportedCsa = false;
    // Opt-in: requests SCHED_FIFO + mlockall for the cycle thread. Off by
    // default -- on some systems (seen on a Beckhoff CX9020) a hardcoded
    // real-time priority for this thread can starve the kernel's own
    // EtherCAT master thread or NIC packet processing instead of helping,
    // so this needs to be validated per-system rather than assumed safe.
    bool realtime = false;
};

// Parses argv via cxxopts into a validated Config.
// Returns std::nullopt if parsing failed or --help was requested (in which
// case usage text has already been printed and the caller should exit(2)).
// Throws std::invalid_argument for validation failures (bad port/frequency/etc).
std::optional<Config> ParseArgs(int argc, char** argv);

// Recommended minimum cycle frequency for the compiled-in backend, or 0 if
// there's no strong recommendation (SOEM handles SDO reads as separate
// blocking calls during configure(), decoupled from cycle rate). For IGH,
// mailbox/SDO transactions -- including the automatic ones IGH's own bus
// scan performs -- ride on the cyclic exchange; too slow a cycle rate can
// let their internal timeouts expire, which stalls the master's kernel-side
// state machine (seen: it spins instead of sleeping, pinning a CPU core,
// and the master never reaches stable OP).
uint32_t RecommendedMinFrequencyHz();

} // namespace ecmqtt
