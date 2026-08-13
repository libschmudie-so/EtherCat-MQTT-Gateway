#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/common.h>

#include "ecmqtt/pdo_override.hpp"

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
    // Use each slave's persistent SII "Configured Station Alias" (falls back
    // to ring position for slaves with none set) instead of ring position
    // for MQTT topics -- see --write-alias for how to set one.
    bool useReportedCsa = false;
    // Opt-in: requests SCHED_FIFO + mlockall for the cycle thread. Off by
    // default -- on some systems (seen on a Beckhoff CX9020) a hardcoded
    // real-time priority for this thread can starve the kernel's own
    // EtherCAT master thread or NIC packet processing instead of helping,
    // so this needs to be validated per-system rather than assumed safe.
    bool realtime = false;

    // How long, in milliseconds, activate() waits for every slave to reach
    // OPERATIONAL before giving up: SOEM fails the whole startup on
    // timeout, IGH just warns and continues (see each backend's activate()
    // for why the two differ). 2s is generous for a normal transition but
    // can be too short on a large/slow-to-settle bus, especially right
    // after a hotplug reconfigure.
    uint32_t opWaitTimeoutMs = 2000;

    // Raw --pdo-config path, if given; main.cpp loads it and resolves entry
    // content from ESI into pdoOverrides before calling backend->configure().
    std::optional<std::string> pdoConfigPath;
    std::vector<PdoOverride> pdoOverrides;

    // Raw --write-alias spec, if given ("<ringPos>=<alias>[,...]"). Presence
    // of this switches main() into a standalone alias-write tool mode that
    // exits immediately after, instead of running the bridge.
    std::optional<std::string> writeAlias;

    // Opt-in: periodically checks for a live topology change (slaves
    // hot-plugged/removed) and reconfigures to pick it up, republishing
    // MQTT metadata for the current slave set. Off by default since it has
    // a real cost even when nothing changes (periodic polling) and, when a
    // change IS detected, briefly pauses cyclic servicing for every slave
    // (not just the one that changed) while reconfiguring -- see
    // IEtherCatBackend::reconfigure(). Only IGH supports this; ignored with
    // a warning on backends where IEtherCatBackend::supportsHotplug() is
    // false.
    bool hotplug = false;

    // Opt-in, only meaningful together with hotplug: when a reconfigure
    // drops a slave that was previously known, also clear its retained MQTT
    // state (metadata topic and any retained process-data topics) and
    // unsubscribe from its output topics, instead of leaving them retained
    // indefinitely under a CSA nothing will ever publish to again.
    bool hotplugCleanup = false;
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
