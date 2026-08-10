#include <algorithm>
#include <atomic>
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
    std::string esiCachePath = [] {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.cache/ecmqtt/esi_cache.json";
    }();

    auto backend = ecmqtt::createBackend();
    try {
        spdlog::info("Configuring EtherCAT master (this may take a few seconds)...");
        backend->configure(cfg);
    } catch (const std::exception& ex) {
        spdlog::critical("Failed to configure EtherCAT master: {}", ex.what());
        return 1;
    }

    // Devices already seen on a previous run are cached (see saveCacheFile
    // below), so a slave we already resolved needs no ESI directory access
    // at all -- only slaves still unknown after the cache is loaded get
    // added to the filter, and loadDirectory() is skipped entirely if that
    // leaves nothing to look up.
    ecmqtt::EsiRepository esiRepo;
    esiRepo.loadCacheFile(esiCachePath);

    ecmqtt::EsiFilter esiFilter;
    for (auto& ds : backend->slaves()) {
        if (esiRepo.find(ds.vendorId, ds.productCode, ds.revisionNo)) continue; // already known
        esiFilter.vendorIds.insert(ds.vendorId);
        esiFilter.deviceKeys.insert(ecmqtt::EsiFilter::MakeDeviceKey(ds.productCode, ds.revisionNo));
    }

    if (!esiFilter.deviceKeys.empty())
        esiRepo.loadDirectory(esiDir, &esiFilter);

    esiRepo.saveCacheFile(esiCachePath);

    // Resolve slave-level name/description via ESI, and cross-reference each
    // live PDO entry's name/dataType by (index, subIndex) when a match exists.
    std::vector<ecmqtt::SlaveDevice> devices;
    devices.reserve(backend->slaves().size());
    for (auto& ds : backend->slaves()) {
        const ecmqtt::EsiDevice* esiDevice = esiRepo.find(ds.vendorId, ds.productCode, ds.revisionNo);
        std::string name = esiDevice && !esiDevice->name.empty() ? esiDevice->name : ds.liveName;
        std::string description = esiDevice && !esiDevice->description.empty() ? esiDevice->description : name;

        for (auto& v : ds.variables) {
            const ecmqtt::EsiPdoEntry* entry = esiDevice ? esiDevice->findEntry(v.index, v.subIndex) : nullptr;
            if (entry) {
                if (!entry->name.empty()) v.name = entry->name;
                if (entry->dataType != ecmqtt::EthercatDataType::Unknown) v.dataType = entry->dataType;
            } else if (v.name.empty()) {
                v.name = fmt::format("{:04X}:{:02X}", v.index, v.subIndex);
            }
        }

        devices.emplace_back(ds, std::move(name), std::move(description));
    }
    spdlog::info("Discovered {} slave(s)", devices.size());

    std::mutex ecMutex;
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

        auto devIt = std::find_if(devices.begin(), devices.end(),
                                   [&](ecmqtt::SlaveDevice& d) { return d.GetCsa(cfg.useReportedCsa) == csa; });
        if (devIt == devices.end()) return;

        auto outVars = devIt->GetOutputVariables();
        auto varIt = std::find_if(outVars.begin(), outVars.end(), [&](ecmqtt::SlaveVariable* v) {
            return v->index == index && v->subIndex == sub;
        });
        if (varIt == outVars.end()) return;

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[topic] = strVal;
        }
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            pendingWrites.push_back({csa, index, sub, std::move(value)});
        }
    });

    mqtt.setOnConnect([&]() {
        spdlog::info("MQTT (re)connected; publishing metadata and subscribing to outputs");
        mqtt.publish(statusTopic, "online", 1, true);

        nlohmann::json slaveSummary = nlohmann::json::object();
        for (auto& dev : devices) {
            auto meta = dev.GetMetadata();

            std::string metaTopic = fmt::format("{}/{}/metadata", cfg.topic, dev.GetCsa(cfg.useReportedCsa));
            mqtt.publish(metaTopic, meta.dump(), 1, true);

            std::string outFilter = fmt::format("{}/{}/+/+", cfg.topic, dev.GetCsa(cfg.useReportedCsa));
            mqtt.subscribe(outFilter, 1);

            slaveSummary[std::to_string(dev.GetCsa())] = {
                {"name", meta["name"]},
                {"description", meta["description"]},
                {"reportedCsa", meta["reportedCsa"]},
                {"ringCsa", meta["ringCsa"]},
            };
        }

        nlohmann::json info = {
            {"interface", cfg.interface},
            {"frequency_hz", cfg.frequencyHz},
            {"esi_path", esiDir},
            {"topic_csa_mode", cfg.useReportedCsa ? "reportedCsa" : "ringCsa"},
            {"slaves", slaveSummary},
        };
        mqtt.publish(cfg.topic + "/bridge/info", info.dump(), 1, true);
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

    spdlog::info("Press Ctrl+C to exit.");

    uint64_t overruns = 0;
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
            {
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
            }

            backend->updateIO();

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
    return 0;
}
