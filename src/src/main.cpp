#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "ecmqtt/config.hpp"
#include "ecmqtt/esi_repository.hpp"
#include "ecmqtt/ethercat_backend.hpp"
#include "ecmqtt/logging.hpp"
#include "ecmqtt/mqtt_client.hpp"
#include "ecmqtt/pdo_override.hpp"
#include "ecmqtt/slave_device.hpp"

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct WriteRequest {
    uint16_t csa;
    uint16_t index;
    uint8_t subIndex;
    nlohmann::json value;
};

std::string MakeTopic(const std::string& root, uint16_t csa, uint16_t index, uint8_t subIndex) {
    return fmt::format("{}/{}/{:04X}/{:02X}", root, csa, index, subIndex);
}

// Slaves with no CoE mailbox (common on basic Beckhoff I/O terminals, e.g.
// EL4001) can't be enumerated live by the SOEM backend, which hands back one
// opaque whole-buffer placeholder per direction instead. Such slaves still
// have a fixed, non-configurable PDO mapping though -- the one declared in
// their ESI file -- so when a match is found, replace the placeholder with
// real per-entry fields by walking the ESI device's PDOs in order and
// accumulating bit offsets from the placeholder's own base pointer, the same
// technique used for live CoE PDO-assignment enumeration. If the ESI mapping
// doesn't fit inside the buffer the backend actually mapped, something's off
// (revision mismatch, non-default mapping) -- keep the opaque placeholder
// rather than risk reading past the end of it.
void ExpandOpaqueFromEsi(ecmqtt::DiscoveredSlave& ds, const ecmqtt::EsiDevice* esiDevice) {
    if (!esiDevice) return;
    if (std::none_of(ds.variables.begin(), ds.variables.end(), [](auto& v) { return v.opaque; })) return;

    std::vector<ecmqtt::SlaveVariable> expanded;
    expanded.reserve(ds.variables.size());

    for (auto& placeholder : ds.variables) {
        if (!placeholder.opaque) {
            expanded.push_back(placeholder);
            continue;
        }

        uint32_t bitOffsetAccum = 0;
        std::vector<ecmqtt::SlaveVariable> fields;
        for (auto& pdo : esiDevice->pdos) {
            if (pdo.direction != placeholder.direction) continue;
            for (auto& entry : pdo.entries) {
                ecmqtt::SlaveVariable v;
                v.index = entry.index;
                v.subIndex = entry.subIndex;
                v.bitLength = entry.bitLen;
                v.direction = placeholder.direction;
                v.dataPtr = placeholder.dataPtr + (bitOffsetAccum / 8);
                v.bitOffset = static_cast<uint8_t>(bitOffsetAccum % 8);
                v.name = entry.name.empty() ? fmt::format("{:04X}:{:02X}", entry.index, entry.subIndex) : entry.name;
                v.description = entry.description;
                v.dataType = entry.dataType != ecmqtt::EthercatDataType::Unknown
                                  ? entry.dataType
                                  : ecmqtt::GuessDataTypeFromBitLength(entry.bitLen);
                bitOffsetAccum += entry.bitLen;
                fields.push_back(std::move(v));
            }
        }

        if (fields.empty() || bitOffsetAccum > placeholder.bitLength) {
            spdlog::debug("ESI PDO mapping for a slave doesn't fit its {}-bit buffer; keeping raw fallback",
                          placeholder.bitLength);
            expanded.push_back(placeholder);
            continue;
        }

        for (auto& f : fields) expanded.push_back(std::move(f));
    }

    ds.variables = std::move(expanded);
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// Lists every PDO ESI declares for a device+direction, as "index" or
// "index \"name\"", for the no-match warning below -- so finding the right
// selector is "read the warning" rather than "go open the ESI XML".
std::string AvailablePdoOptions(const ecmqtt::EsiDevice& dev, ecmqtt::DataDirection dir) {
    std::string out;
    for (auto& p : dev.pdos) {
        if (p.direction != dir) continue;
        if (!out.empty()) out += ", ";
        out += fmt::format("{:#06x}", p.index);
        if (!p.name.empty()) out += fmt::format(" \"{}\"", p.name);
    }
    return out.empty() ? "(none declared for this direction)" : out;
}

// Fills in .rxPdos/.txPdos (index + entries) for each override by matching
// its (vendorId, productCode, revisionNo) against a loaded ESI device and
// resolving each requested RxPdo/TxPdo selector -- a hex/decimal index or a
// declared ESI name (e.g. an EL3012's "Full"/"Simple"), matched
// case-insensitively -- against that device's PDOs. Leaves a PDO's entries
// empty (backends then fall back to the slave's own default entries for
// that PDO index, still selecting the right one) when no ESI match is
// found -- logged, not fatal, since the assignment itself can still be
// attempted on the wire.
void ResolvePdoOverrides(std::vector<ecmqtt::PdoOverride>& overrides, ecmqtt::EsiRepository& esiRepo) {
    auto resolveDirection = [](const ecmqtt::EsiDevice* dev, const std::vector<std::string>& selectors,
                                ecmqtt::DataDirection dir, std::vector<ecmqtt::PdoOverridePdo>& out) {
        for (const std::string& selector : selectors) {
            ecmqtt::PdoOverridePdo pdo;
            // 0 is never a real PDO index -- ParseEsiNumber()'s fallback
            // for a selector that isn't numeric at all (a name). Set this
            // unconditionally, ESI or not, so a numeric selector still
            // gets attempted blind on the wire without an ESI match, same
            // as before name-based selectors existed.
            pdo.pdoIndex = static_cast<uint16_t>(ecmqtt::ParseEsiNumber(selector));

            const ecmqtt::EsiPdo* match = nullptr;
            if (dev) {
                if (pdo.pdoIndex != 0)
                    for (auto& p : dev->pdos)
                        if (p.index == pdo.pdoIndex && p.direction == dir) { match = &p; break; }
                if (!match)
                    for (auto& p : dev->pdos)
                        if (p.direction == dir && !p.name.empty() && EqualsIgnoreCase(p.name, selector)) {
                            match = &p;
                            break;
                        }
            }
            if (match) {
                pdo.pdoIndex = match->index; // resolves a name selector to its real numeric index too
                for (auto& e : match->entries) pdo.entries.push_back({e.index, e.subIndex, e.bitLen});
                spdlog::debug("--pdo-config: '{}' resolved to PDO {:#06x} ({} entries: {})", selector, match->index,
                              match->entries.size(), [&] {
                                  std::string s;
                                  for (auto& e : match->entries)
                                      s += fmt::format("{}{:#06x}:{:#04x}/{}", s.empty() ? "" : ", ", e.index,
                                                        e.subIndex, e.bitLen);
                                  return s;
                              }());
            } else if (dev) {
                spdlog::warn(
                    "--pdo-config: '{}' not found in ESI for vendor {:#x} product {:#x} rev {:#x}. Available: {}",
                    selector, dev->vendorId, dev->productCode, dev->revisionNo, AvailablePdoOptions(*dev, dir));
            } else if (pdo.pdoIndex == 0) {
                spdlog::warn(
                    "--pdo-config: '{}' isn't a numeric PDO index and there's no ESI match to resolve it by "
                    "name against; this selector will be dropped",
                    selector);
            }
            out.push_back(std::move(pdo));
        }
    };

    for (auto& ov : overrides) {
        const ecmqtt::EsiDevice* dev = esiRepo.resolve(ov.vendorId, ov.productCode, ov.revisionNo);
        if (!dev)
            spdlog::warn(
                "--pdo-config: no ESI match for vendor {:#x} product {:#x} rev {:#x}; the requested PDO "
                "assignment will still be attempted on the wire, but field names/types won't be resolved",
                ov.vendorId, ov.productCode, ov.revisionNo);

        resolveDirection(dev, ov.rxPdoSelectors, ecmqtt::DataDirection::Output, ov.rxPdos);
        resolveDirection(dev, ov.txPdoSelectors, ecmqtt::DataDirection::Input, ov.txPdos);
    }
}

// Parses "<ringPos>=<alias>[,<ringPos>=<alias>...]" for --write-alias.
// Accepts decimal or 0x-prefixed hex for the alias value.
std::vector<std::pair<uint16_t, uint16_t>> ParseAliasSpec(const std::string& spec) {
    std::vector<std::pair<uint16_t, uint16_t>> result;
    size_t start = 0;
    while (start < spec.size()) {
        size_t comma = spec.find(',', start);
        std::string item = spec.substr(start, comma - start);

        size_t eq = item.find('=');
        if (eq == std::string::npos)
            throw std::invalid_argument("--write-alias: expected <ringPos>=<alias>, got '" + item + "'");

        uint16_t ringPos = static_cast<uint16_t>(std::stoul(item.substr(0, eq)));
        uint16_t alias = static_cast<uint16_t>(std::stoul(item.substr(eq + 1), nullptr, 0));
        result.emplace_back(ringPos, alias);

        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

// Opt-in via --realtime (see Config::realtime). In principle this protects
// against slave sync-manager watchdog trips caused by normal SCHED_OTHER
// scheduling jitter -- but a hardcoded high SCHED_FIFO priority can instead
// starve a system's own EtherCAT master kernel thread or NIC packet
// processing if they aren't prioritized to cope with it (observed on a
// Beckhoff CX9020 with the IGH backend: the master stopped receiving any
// frames at all once this was enabled). Validate on your actual target
// before relying on it; it is not a safe-by-default improvement.
void TryEnableRealtimeScheduling() {
    sched_param sp{};
    sp.sched_priority = 80;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        spdlog::warn(
            "Could not set SCHED_FIFO priority for the EtherCAT cycle thread (need CAP_SYS_NICE/root "
            "and an adequate RLIMIT_RTPRIO); cycle timing may jitter under load and trip slave watchdogs");
    } else {
        spdlog::info("EtherCAT cycle thread running at SCHED_FIFO priority {}", sp.sched_priority);
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        spdlog::warn("Could not mlockall(); page faults may add cycle jitter");
}

} // namespace

int main(int argc, char** argv) {
    std::optional<ecmqtt::Config> parsed;
    try {
        parsed = ecmqtt::ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        fmt::print(stderr, "Error: {}\n", ex.what());
        return 2;
    }
    if (!parsed) return 2; // --help or a parse error already reported

    ecmqtt::Config cfg = std::move(*parsed);
    ecmqtt::InitLogging(cfg.logLevel);

    // --write-alias switches into a standalone tool mode: write the
    // requested aliases and exit, never touching MQTT or the cycle loop.
    if (cfg.writeAlias) {
        std::vector<std::pair<uint16_t, uint16_t>> pairs;
        try {
            pairs = ParseAliasSpec(*cfg.writeAlias);
        } catch (const std::exception& ex) {
            spdlog::critical("{}", ex.what());
            return 2;
        }
        auto backend = ecmqtt::createBackend();
        return backend->writeAliases(cfg, pairs) ? 0 : 1;
    }

    spdlog::info(
        "Starting ethercat-mqtt-gateway: iface={} broker={}:{} esi={} freq={}Hz retain={} topic={} clientId={} "
        "useReportedCsa={}",
        cfg.interface, cfg.broker, cfg.port, cfg.esiDir.value_or("<default>"), cfg.frequencyHz,
        cfg.retainProcessData, cfg.topic, cfg.clientId, cfg.useReportedCsa);

    if (auto minFreq = ecmqtt::RecommendedMinFrequencyHz(); minFreq > 0 && cfg.frequencyHz < minFreq) {
        spdlog::warn(
            "--frequency {}Hz is below the recommended minimum of {}Hz for this backend: mailbox/SDO "
            "transactions ride on the cyclic exchange and can time out below that, which can stall the "
            "master's internal state machine (seen: pinned CPU in a kernel thread, master never reaching "
            "stable OP). Raise --frequency if slaves don't reach OP cleanly.",
            cfg.frequencyHz, minFreq);
    }

    std::string esiDir = cfg.esiDir.value_or([] {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.local/share/ESI";
    }());
    std::string esiIndexPath = [] {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.cache/ecmqtt/esi_index.json";
    }();

    // The index (path -> content hash -> device identities it declares)
    // lets refreshIndex() skip re-parsing any ESI file that hasn't changed
    // since last run, without duplicating that file's actual content into
    // the cache -- resolve() parses the one owning file on demand, the
    // first time a given device is actually needed.
    ecmqtt::EsiRepository esiRepo;
    esiRepo.loadIndex(esiIndexPath);
    esiRepo.refreshIndex(esiDir);
    esiRepo.saveIndex(esiIndexPath);

    if (cfg.pdoConfigPath) {
        try {
            cfg.pdoOverrides = ecmqtt::LoadPdoOverrideConfig(*cfg.pdoConfigPath);
        } catch (const std::exception& ex) {
            spdlog::critical("Failed to load --pdo-config '{}': {}", *cfg.pdoConfigPath, ex.what());
            return 1;
        }

        // Both backends need each override's PDO entries resolved from ESI
        // *before* configure() returns -- SOEM applies the assignment during
        // its PRE-OP transition hook, IGH hands entries straight to
        // ecrt_slave_config_pdos().
        ResolvePdoOverrides(cfg.pdoOverrides, esiRepo);
    }

    auto backend = ecmqtt::createBackend();
    try {
        spdlog::info("Configuring EtherCAT master (this may take a few seconds)...");
        backend->configure(cfg, esiRepo);
    } catch (const std::exception& ex) {
        spdlog::critical("Failed to configure EtherCAT master: {}", ex.what());
        return 1;
    }

    std::mutex ecMutex;

    // Resolves slave-level name/description via ESI, cross-references each
    // live PDO entry's name/dataType by (index, subIndex) when a match
    // exists, and wraps everything as SlaveDevice. Used for the initial
    // discovery below and again after a hotplug reconfigure() picks up a
    // new bus topology -- callers must hold ecMutex before replacing
    // `devices` with this function's result, since the MQTT thread reads it
    // concurrently (see setOnMessage/publishMetadataAndSubscribe below).
    auto buildDevices = [&]() {
        std::vector<ecmqtt::SlaveDevice> result;
        result.reserve(backend->slaves().size());
        for (auto& ds : backend->slaves()) {
            const ecmqtt::EsiDevice* esiDevice = esiRepo.resolve(ds.vendorId, ds.productCode, ds.revisionNo);
            std::string name = esiDevice && !esiDevice->name.empty() ? esiDevice->name : ds.liveName;
            // Neither ESI nor the backend's own live introspection gave us
            // anything to call this device -- fall back to its raw identity
            // rather than publishing an empty string, so it's at least
            // possible to tell which physical device this is and go find
            // (or add) its ESI file.
            if (name.empty()) {
                name = fmt::format("{:#x}:{:#x}:{:#x}", ds.vendorId, ds.productCode, ds.revisionNo);
                spdlog::warn("No ESI match and no live name for slave at ring position {} (vendor {:#x} product "
                             "{:#x} rev {:#x})",
                             ds.ringCsa, ds.vendorId, ds.productCode, ds.revisionNo);
            }
            std::string description = esiDevice && !esiDevice->description.empty() ? esiDevice->description : name;

            ExpandOpaqueFromEsi(ds, esiDevice);

            for (auto& v : ds.variables) {
                const ecmqtt::EsiPdoEntry* entry = esiDevice ? esiDevice->findEntry(v.index, v.subIndex) : nullptr;
                if (entry) {
                    if (!entry->name.empty()) v.name = entry->name;
                    if (!entry->description.empty()) v.description = entry->description;
                    if (entry->dataType != ecmqtt::EthercatDataType::Unknown) v.dataType = entry->dataType;
                } else if (v.name.empty()) {
                    v.name = fmt::format("{:04X}:{:02X}", v.index, v.subIndex);
                }
            }

            std::vector<ecmqtt::AvailablePdo> availableRxPdos, availableTxPdos;
            if (esiDevice) {
                for (auto& p : esiDevice->pdos) {
                    auto& target = p.direction == ecmqtt::DataDirection::Output ? availableRxPdos : availableTxPdos;
                    target.push_back({p.index, p.name});
                }
            }

            result.emplace_back(ds, std::move(name), std::move(description), std::move(availableRxPdos),
                                 std::move(availableTxPdos));
        }
        return result;
    };

    std::vector<ecmqtt::SlaveDevice> devices = buildDevices();
    spdlog::info("Discovered {} slave(s)", devices.size());
    std::mutex cacheMutex;
    std::unordered_map<std::string, std::string> cache;
    std::mutex writeMutex;
    std::deque<WriteRequest> pendingWrites;

    std::string statusTopic = cfg.topic + "/bridge/status";
    ecmqtt::MqttClient mqtt(cfg.clientId, statusTopic, "offline");

    mqtt.setOnMessage([&](const std::string& topic, const std::string& payload) {
        std::string prefix = cfg.topic + "/";
        if (topic.rfind(prefix, 0) != 0) return;

        std::string remainder = topic.substr(prefix.size());
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t pos = remainder.find('/', start);
            parts.push_back(remainder.substr(start, pos - start));
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        if (parts.size() != 3) return;

        uint16_t csa, index;
        uint8_t sub;
        try {
            csa = static_cast<uint16_t>(std::stoul(parts[0], nullptr, 10));
            index = static_cast<uint16_t>(std::stoul(parts[1], nullptr, 16));
            sub = static_cast<uint8_t>(std::stoul(parts[2], nullptr, 16));
        } catch (const std::exception&) {
            return;
        }

        nlohmann::json value;
        try {
            value = nlohmann::json::parse(payload);
        } catch (const nlohmann::json::parse_error&) {
            value = payload;
        }
        std::string strVal = value.dump();

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = cache.find(topic);
            if (it != cache.end() && it->second == strVal) return;
        }

        // devices can be replaced wholesale by a hotplug reconfigure on the
        // main cycle thread (see buildDevices() above); guard access with
        // the same mutex that protects it there.
        bool found;
        {
            std::lock_guard<std::mutex> lock(ecMutex);
            auto devIt = std::find_if(devices.begin(), devices.end(),
                                       [&](ecmqtt::SlaveDevice& d) { return d.GetCsa(cfg.useReportedCsa) == csa; });
            found = devIt != devices.end();
            if (found) {
                auto outVars = devIt->GetOutputVariables();
                auto varIt = std::find_if(outVars.begin(), outVars.end(), [&](ecmqtt::SlaveVariable* v) {
                    return v->index == index && v->subIndex == sub;
                });
                found = varIt != outVars.end();
            }
        }
        if (!found) return;

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[topic] = strVal;
        }
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            pendingWrites.push_back({csa, index, sub, std::move(value)});
        }
    });

    // Publishes retained metadata + subscribes to output topics for every
    // current slave, and republishes bridge/info. Called on every MQTT
    // (re)connect, and again before/after a hotplug reconfigure picks up a
    // new topology (see the "state" field below). Only takes ecMutex long
    // enough to snapshot what's needed from `devices` -- the actual
    // publish/subscribe calls (network I/O) run unlocked, same reasoning as
    // decoupling MQTT from the EtherCAT cycle thread elsewhere: a slow
    // broker must never hold up updateIO().
    auto publishMetadataAndSubscribe = [&](const std::string& state = "running") {
        struct Entry {
            uint16_t csa;
            nlohmann::json meta;
        };
        std::vector<Entry> snapshot;
        {
            std::lock_guard<std::mutex> lock(ecMutex);
            snapshot.reserve(devices.size());
            for (auto& dev : devices) snapshot.push_back({dev.GetCsa(cfg.useReportedCsa), dev.GetMetadata()});
        }

        spdlog::info("Publishing metadata and subscribing to outputs for {} slave(s)", snapshot.size());
        mqtt.publish(statusTopic, "online", 1, true);

        nlohmann::json slaveSummary = nlohmann::json::object();
        for (auto& [csa, meta] : snapshot) {
            std::string metaTopic = fmt::format("{}/{}/metadata", cfg.topic, csa);
            mqtt.publish(metaTopic, meta.dump(), 1, true);

            std::string outFilter = fmt::format("{}/{}/+/+", cfg.topic, csa);
            mqtt.subscribe(outFilter, 1);

            slaveSummary[std::to_string(meta["ringCsa"].get<uint16_t>())] = {
                {"name", meta["name"]},
                {"description", meta["description"]},
                {"state", meta["state"]},
                {"error", meta["error"]},
                {"alarmCode", meta["alarmCode"]},
                {"alarm", meta["alarm"]},
                {"reportedCsa", meta["reportedCsa"]},
                {"ringCsa", meta["ringCsa"]},
            };
        }

        nlohmann::json info = {
            {"state", state}, // "running", or during a hotplug reconfigure: "rescanning" then "waiting"
            {"interface", cfg.interface},
            {"frequency_hz", cfg.frequencyHz},
            {"esi_path", esiDir},
            {"topic_csa_mode", cfg.useReportedCsa ? "reportedCsa" : "ringCsa"},
            {"slaves", slaveSummary},
        };
        mqtt.publish(cfg.topic + "/bridge/info", info.dump(), 1, true);
    };

    mqtt.setOnConnect([&]() {
        spdlog::info("MQTT (re)connected");
        publishMetadataAndSubscribe();
    });

    spdlog::info("Connecting to MQTT broker at {}:{}...", cfg.broker, cfg.port);
    if (!mqtt.connect(cfg.broker, cfg.port, /*maxAttempts=*/5, std::chrono::seconds(2))) {
        spdlog::critical("Unable to connect to MQTT broker after retries");
        backend->shutdown();
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::chrono::duration<double, std::milli> periodMs(1000.0 / cfg.frequencyHz);
    auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(periodMs);

    // MQTT publishing (network I/O, JSON serialization for potentially many
    // topics) runs on its own thread, fed only the already-extracted (topic,
    // value) snapshot -- so a slow broker/network can never delay the next
    // updateIO() call and trip a slave's sync-manager watchdog. Only the
    // latest snapshot is kept: if the publisher falls behind, we coalesce
    // to the newest state rather than growing an unbounded backlog.
    std::mutex snapshotMutex;
    std::condition_variable snapshotCv;
    std::vector<std::pair<std::string, nlohmann::json>> pendingSnapshot;
    bool snapshotReady = false;
    bool publisherStop = false;

    std::thread publisherThread([&] {
        std::vector<std::pair<std::string, nlohmann::json>> local;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(snapshotMutex);
                snapshotCv.wait(lock, [&] { return snapshotReady || publisherStop; });
                if (publisherStop && !snapshotReady) break;
                local = std::move(pendingSnapshot);
                snapshotReady = false;
            }
            for (auto& [topic, value] : local) {
                std::string strVal = value.dump();
                bool changed;
                {
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    auto it = cache.find(topic);
                    changed = (it == cache.end() || it->second != strVal);
                    if (changed) cache[topic] = strVal;
                }
                if (changed) mqtt.publish(topic, strVal, 1, cfg.retainProcessData);
            }
        }
    });

    if (cfg.realtime)
        TryEnableRealtimeScheduling();

    // Bring slaves to OPERATIONAL as the very last setup step, right before
    // the cyclic loop starts: everything slow (MQTT connect, ESI parsing)
    // already happened above. A slave's sync-manager watchdog -- and, for
    // IGH, pending mailbox/SDO exchanges from the bus scan riding along on
    // the cyclic exchange -- needs the master to start cycling reliably
    // very soon after activation, not after several more seconds of
    // unrelated setup work.
    try {
        backend->activate();
    } catch (const std::exception& ex) {
        spdlog::critical("Failed to activate EtherCAT master: {}", ex.what());
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            publisherStop = true;
        }
        snapshotCv.notify_one();
        publisherThread.join();
        return 1;
    }

    // The very first publishMetadataAndSubscribe() call (via setOnConnect,
    // above) necessarily ran before activate() -- each slave's alState is
    // only known afterward. Republish now that it is, so "state" in
    // retained metadata reflects reality instead of permanently reading
    // Unknown on a run that never hits a hotplug reconfigure (the only
    // other thing that republishes).
    publishMetadataAndSubscribe();

    spdlog::info("Press Ctrl+C to exit.");

    if (cfg.hotplug && !backend->supportsHotplug())
        spdlog::warn("--hotplug was requested but this backend doesn't support it; ignoring");

    int exitCode = 0;
    uint64_t overruns = 0;
    uint64_t cycleCount = 0;
    // What was last published, so the check below can tell whether
    // anything actually needs republishing -- both backends now keep
    // alState/alStatusCode fresh every single updateIO() call for free (see
    // IghBackend::RefreshSlaveStatesRt()/SoemBackend::RefreshSlaveStates()),
    // so this deliberately does NOT call anything new here: just compares
    // already-current in-memory fields against this cache. Unlike an
    // earlier version of this check (reverted -- see git history), nothing
    // here calls ecrt_master_get_slave() or any other backend query, so it
    // doesn't reproduce that regression.
    std::vector<std::pair<ecmqtt::SlaveAlState, uint16_t>> lastPublishedStates;
    constexpr uint64_t kStatePublishCheckCycles = 100;
    auto lastHotplugCheck = std::chrono::steady_clock::now();
    // First tick fires immediately (not after a full period) so the first
    // updateIO() call lands as soon as possible after activate().
    auto nextTick = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(nextTick);
        nextTick += period;
        auto cycleStart = std::chrono::steady_clock::now();

        std::vector<std::pair<std::string, nlohmann::json>> snapshot;
        snapshot.reserve(256);
        {
            std::lock_guard<std::mutex> lock(ecMutex);

            // Passed to the backend so it can apply pending writes at
            // exactly the right point in its own cyclic exchange -- see
            // IEtherCatBackend::updateIO() -- rather than main.cpp guessing
            // an ordering that only happens to work for one backend.
            backend->updateIO([&] {
                std::deque<WriteRequest> local;
                {
                    std::lock_guard<std::mutex> wlock(writeMutex);
                    local.swap(pendingWrites);
                }
                for (auto& req : local) {
                    auto devIt = std::find_if(devices.begin(), devices.end(), [&](ecmqtt::SlaveDevice& d) {
                        return d.GetCsa(cfg.useReportedCsa) == req.csa;
                    });
                    if (devIt == devices.end()) continue;

                    auto outVars = devIt->GetOutputVariables();
                    auto varIt = std::find_if(outVars.begin(), outVars.end(), [&](ecmqtt::SlaveVariable* v) {
                        return v->index == req.index && v->subIndex == req.subIndex;
                    });
                    if (varIt == outVars.end()) continue;

                    try {
                        devIt->WriteVariableAsJson(**varIt, req.value);
                    } catch (const std::exception& ex) {
                        spdlog::warn("Failed to write CSA={} {:04X}/{:02X} '{}': {}", req.csa, req.index,
                                     req.subIndex, req.value.dump(), ex.what());
                    }
                }
            });

            for (auto& dev : devices) {
                auto vars = cfg.noPublishOutputs ? dev.GetInputVariables() : dev.GetAllVariables();
                for (auto* v : vars) {
                    snapshot.emplace_back(MakeTopic(cfg.topic, dev.GetCsa(cfg.useReportedCsa), v->index, v->subIndex),
                                           dev.ReadVariableAsJson(*v));
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            pendingSnapshot = std::move(snapshot);
            snapshotReady = true;
        }
        snapshotCv.notify_one();

        auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed > period + std::chrono::milliseconds(2)) {
            ++overruns;
            spdlog::warn("Cycle overrun: elapsed={:.2f}ms > period={:.2f}ms (count={})",
                         std::chrono::duration<double, std::milli>(elapsed).count(),
                         std::chrono::duration<double, std::milli>(period).count(), overruns);
        }

        if (++cycleCount % kStatePublishCheckCycles == 0) {
            bool changed;
            {
                std::lock_guard<std::mutex> lock(ecMutex);
                auto& liveSlaves = backend->slaves();
                changed = lastPublishedStates.size() != liveSlaves.size();
                if (!changed)
                    for (size_t i = 0; i < liveSlaves.size(); ++i)
                        if (lastPublishedStates[i].first != liveSlaves[i].alState ||
                            lastPublishedStates[i].second != liveSlaves[i].alStatusCode) {
                            changed = true;
                            break;
                        }
                if (changed) {
                    lastPublishedStates.clear();
                    for (auto& s : liveSlaves) lastPublishedStates.emplace_back(s.alState, s.alStatusCode);
                }
            }
            if (changed) publishMetadataAndSubscribe();
        }

        if (cfg.hotplug && backend->supportsHotplug()) {
            auto hpNow = std::chrono::steady_clock::now();
            if (hpNow - lastHotplugCheck >= std::chrono::seconds(2)) {
                lastHotplugCheck = hpNow;
                if (backend->topologyChanged()) {
                    spdlog::info(
                        "EtherCAT topology change detected; reconfiguring (all slaves briefly pause)...");
                    publishMetadataAndSubscribe("rescanning");

                    // A hotplug reconfigure deactivates and reactivates the
                    // whole master -- IGH gets a brand-new domain, so every
                    // output byte comes back zeroed/default instead of
                    // whatever was last written. Snapshot the current value
                    // of every output variable now, while `devices` (and
                    // the backend storage it points into) is still the
                    // live pre-reconfigure state, and re-queue them as
                    // ordinary writes below once the new topology is up --
                    // same path an MQTT-triggered write already takes, so
                    // whatever's physically being controlled doesn't
                    // silently reset just because some other slave on the
                    // bus was added/removed.
                    std::vector<uint16_t> oldCsas;
                    std::deque<WriteRequest> savedOutputs;
                    {
                        std::lock_guard<std::mutex> lock(ecMutex);
                        oldCsas.reserve(devices.size());
                        for (auto& d : devices) {
                            uint16_t csa = d.GetCsa(cfg.useReportedCsa);
                            oldCsas.push_back(csa);
                            for (auto* v : d.GetOutputVariables())
                                savedOutputs.push_back({csa, v->index, v->subIndex, d.ReadVariableAsJson(*v)});
                        }
                    }

                    try {
                        backend->reconfigure(cfg, esiRepo);
                    } catch (const std::exception& ex) {
                        spdlog::critical("Failed to reconfigure EtherCAT master after a topology change: {}",
                                          ex.what());
                        exitCode = 1;
                        break;
                    }

                    // reconfigure() just cleared and repopulated the
                    // backend's own slave storage -- every SlaveDevice in
                    // the *old* `devices` holds a pointer into that now-
                    // destroyed storage. Rebuild `devices` immediately,
                    // before touching it for anything (even just publishing
                    // a "waiting" status): with every slave removed, every
                    // entry would be dangling and this crashes reliably;
                    // with only some removed it's still equally undefined
                    // behavior, just not guaranteed to fault every time.
                    auto newDevices = buildDevices();
                    std::vector<uint16_t> newCsas;
                    newCsas.reserve(newDevices.size());
                    for (auto& d : newDevices) newCsas.push_back(d.GetCsa(cfg.useReportedCsa));
                    {
                        std::lock_guard<std::mutex> lock(ecMutex);
                        devices = std::move(newDevices);
                    }

                    // Safe to publish/subscribe against `devices` here even
                    // though activate() hasn't run yet: SlaveDevice reads
                    // straight through to the backend's live DiscoveredSlave
                    // storage, and GetMetadata() only touches name/
                    // description/state/pdo-list fields, none of which need
                    // a resolved dataPtr (that only matters for actually
                    // reading/writing process data, filtered out of
                    // GetAllVariables() et al. until activate() sets it).
                    publishMetadataAndSubscribe("waiting"); // rescanned; bringing slaves back to OP

                    try {
                        backend->activate();
                    } catch (const std::exception& ex) {
                        spdlog::critical("Failed to reactivate EtherCAT master after a topology change: {}",
                                          ex.what());
                        exitCode = 1;
                        break;
                    }

                    spdlog::info("Reconfigured: {} slave(s) now known", devices.size());

                    if (!savedOutputs.empty()) {
                        spdlog::info("Re-applying {} output value(s) saved before the reconfigure",
                                     savedOutputs.size());
                        std::lock_guard<std::mutex> lock(writeMutex);
                        for (auto& req : savedOutputs) pendingWrites.push_back(std::move(req));
                    }

                    if (cfg.hotplugCleanup) {
                        for (uint16_t csa : oldCsas) {
                            if (std::find(newCsas.begin(), newCsas.end(), csa) != newCsas.end()) continue;

                            mqtt.unsubscribe(fmt::format("{}/{}/+/+", cfg.topic, csa));
                            mqtt.publish(fmt::format("{}/{}/metadata", cfg.topic, csa), "", 1, true);

                            std::string prefix = fmt::format("{}/{}/", cfg.topic, csa);
                            std::vector<std::string> topicsToClear;
                            {
                                std::lock_guard<std::mutex> lock(cacheMutex);
                                for (auto& [topic, value] : cache)
                                    if (topic.rfind(prefix, 0) == 0) topicsToClear.push_back(topic);
                                for (auto& topic : topicsToClear) cache.erase(topic);
                            }
                            for (auto& topic : topicsToClear) mqtt.publish(topic, "", 1, true);

                            spdlog::info("Cleared MQTT state for removed slave CSA={} ({} retained topic(s))", csa,
                                         topicsToClear.size() + 1);
                        }
                    }

                    publishMetadataAndSubscribe("running");

                    // reconfigure() took nontrivial wall-clock time; resume
                    // from now rather than firing a burst of "missed" ticks.
                    nextTick = std::chrono::steady_clock::now();
                }
            }
        }
    }

    spdlog::info("Shutting down...");
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        publisherStop = true;
    }
    snapshotCv.notify_one();
    publisherThread.join();

    mqtt.publish(statusTopic, "offline", 1, true);
    mqtt.disconnect();
    backend->shutdown();
    spdlog::info("Shutdown complete.");
    return exitCode;
}
