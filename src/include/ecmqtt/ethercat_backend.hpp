#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "ecmqtt/config.hpp"
#include "ecmqtt/esi_repository.hpp"
#include "ecmqtt/ethercat_types.hpp"

namespace ecmqtt {

// A single PDO entry mapped into the live process image.
struct SlaveVariable {
    std::string name;              // resolved from ESI, or a numeric fallback
    std::string description;       // resolved from ESI (falls back to name); empty without a match
    uint16_t index = 0;
    uint8_t subIndex = 0;
    uint16_t bitLength = 0;
    uint8_t bitOffset = 0;         // 0-7: bit offset within the byte at dataPtr
    EthercatDataType dataType = EthercatDataType::Unknown;
    DataDirection direction = DataDirection::Input;
    uint8_t* dataPtr = nullptr;    // pointer into the live process image
    // True for a whole-buffer placeholder emitted when a backend couldn't
    // enumerate real PDO entries (e.g. SOEM + a slave with no CoE mailbox).
    // main.cpp expands these into real per-entry fields using the ESI
    // device's declared (fixed) PDO mapping when a match is found.
    bool opaque = false;
};

// A slave discovered on the bus, with its live-mapped PDO variables.
struct DiscoveredSlave {
    uint16_t ringCsa = 0;          // 1-based ring position
    // Persistent SII "Configured Station Alias" if the slave has one set
    // (survives moving the slave in the ring, or swapping it for an
    // identical replacement with the same alias written -- see
    // IEtherCatBackend::writeAliases()); falls back to ringCsa otherwise.
    uint16_t reportedCsa = 0;
    uint32_t vendorId = 0;
    uint32_t productCode = 0;
    uint32_t revisionNo = 0;
    std::string liveName;          // name reported directly by the stack, if any
    std::vector<SlaveVariable> variables;
};

// Backend-agnostic interface implemented by exactly one of
// backends/soem_backend.cpp or backends/igh_backend.cpp, selected at
// compile time via the EC_BACKEND CMake option.
class IEtherCatBackend {
public:
    virtual ~IEtherCatBackend() = default;

    // Opens the interface, scans the bus, and maps PDOs (through PRE-OP/
    // SAFE-OP as needed to read CoE PDO assignment) -- but does not yet
    // demand cyclic servicing from the caller. Throws std::runtime_error on
    // failure. slaves() is valid after this returns.
    //
    // esiRepo is passed in (already pointed at the configured ESI directory
    // by main.cpp) so a backend can fall back to a device's ESI-declared
    // default PDO mapping when its own live introspection can't enumerate
    // one -- see IghBackend::rescan() for the case that needs this (a
    // hardwired-mapping slave with neither a CoE mailbox nor an SII PDO-
    // assignment category). Backends that don't need it (SOEM: its
    // opaque-placeholder fallback is resolved against ESI entirely in
    // main.cpp, after configure() returns) may just ignore it.
    virtual void configure(const Config& cfg, EsiRepository& esiRepo) = 0;

    // Finalizes configuration and brings all slaves toward OPERATIONAL,
    // starting the point at which the master expects the caller to call
    // updateIO() promptly and regularly. Call this immediately before
    // entering the cyclic loop -- any slow work (MQTT connect, ESI parsing,
    // ...) between configure() and here is fine, but a delay *after*
    // activate() before cycling starts can cause slaves to time out waiting
    // for mailbox/SDO responses that ride along on the cyclic exchange (IGH
    // backend), or trip their sync-manager watchdog (both backends).
    virtual void activate() = 0;

    virtual std::vector<DiscoveredSlave>& slaves() = 0;

    // Performs one full cyclic process-data exchange, calling applyWrites()
    // at the correct point in that exchange for this backend to apply
    // pending output writes so they're guaranteed to actually go out this
    // cycle. This isn't the same point for every backend: SOEM sends
    // immediately after applyWrites() runs, so applying writes first (then
    // send, then receive the reply) works. IGH's receive/process step
    // resets the process image from the just-received frame *before* an app
    // is expected to write outputs (its documented cyclic pattern is
    // receive -> process -> read inputs -> write outputs -> queue -> send);
    // applying writes before that reset -- e.g. before calling this at all --
    // gets silently discarded, which looked like "the written value snaps
    // back to the previous one" on real hardware. All variables (including
    // outputs) are safe to read for publishing any time after this call
    // returns, on either backend.
    virtual void updateIO(const std::function<void()>& applyWrites) = 0;

    // Returns all slaves to a safe state and releases the master.
    virtual void shutdown() = 0;

    // True if this backend can detect and apply a live topology change
    // (slaves hot-plugged or removed) without restarting the whole process.
    // SOEM: false -- its classic API has no equivalent notion once
    // configure() has run. IGH: true, though see reconfigure()'s doc
    // comment for the real cost of using it.
    virtual bool supportsHotplug() const { return false; }

    // Checks whether the slaves actually present on the bus right now
    // differ from what slaves() currently reflects. Only meaningful if
    // supportsHotplug() is true; not necessarily cheap (IGH's underlying
    // call is documented "blocking"), so call this occasionally from a
    // low-frequency check, not from inside the tight per-cycle section.
    virtual bool topologyChanged() { return false; }

    // Re-scans the bus and rebuilds the slave/PDO list and domain, picking
    // up whatever's physically present now -- new slaves get registered,
    // gone ones drop out of slaves(). Only meaningful if supportsHotplug()
    // is true. On IGH this is NOT a targeted "just add the new one"
    // operation: IGH's own API docs are explicit that slave configuration
    // can't be altered once ecrt_master_activate() has run, so this
    // deactivates first -- every slave, not just the one that changed,
    // briefly drops cyclic servicing during the call. Each call also leaks
    // one domain object for the process's lifetime (IGH's public API has no
    // domain-free call) -- fine for occasional hotplug events, worth
    // knowing if they happen very frequently. Throws std::runtime_error on
    // failure, same as configure()/activate() -- the caller should treat
    // that as fatal, since the old topology is already gone by the time
    // this can fail.
    virtual void reconfigure(const Config& cfg, EsiRepository& esiRepo) {
        (void)cfg;
        (void)esiRepo;
        throw std::logic_error("reconfigure() not supported by this backend");
    }

    // Standalone tool operation, independent of configure()/activate()/
    // shutdown() (does its own minimal bus scan): writes a persistent SII
    // "Configured Station Alias" to one or more slaves, addressed by their
    // *current* ring position (ringPos -> alias pairs). Returns false if
    // this backend can't do it at all, or if opening the interface/bus scan
    // failed outright; logs a warning per-slave for individual write
    // failures but still attempts the rest. The default implementation logs
    // that this backend has no support and returns false -- only SOEM
    // overrides it (IGH's userspace library has no SII/EEPROM write API;
    // use the target's own 'ethercat alias' tool instead).
    virtual bool writeAliases(const Config& cfg, const std::vector<std::pair<uint16_t, uint16_t>>& ringPosToAlias) {
        (void)cfg;
        (void)ringPosToAlias;
        spdlog::error(
            "This backend has no API for writing a slave's persistent SII station alias. On IGH, use the "
            "target's own 'ethercat alias -pPOSITION VALUE' command-line tool (part of the standard IGH "
            "master install) instead, then power-cycle the slave for it to take effect.");
        return false;
    }
};

// Implemented exactly once, by whichever backend .cpp is compiled in.
std::unique_ptr<IEtherCatBackend> createBackend();

} // namespace ecmqtt
