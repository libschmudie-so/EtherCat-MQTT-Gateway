#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ecmqtt/config.hpp"
#include "ecmqtt/ethercat_types.hpp"

namespace ecmqtt {

// A single PDO entry mapped into the live process image.
struct SlaveVariable {
    std::string name;              // resolved from ESI, or a numeric fallback
    uint16_t index = 0;
    uint8_t subIndex = 0;
    uint16_t bitLength = 0;
    uint8_t bitOffset = 0;         // 0-7: bit offset within the byte at dataPtr
    EthercatDataType dataType = EthercatDataType::Unknown;
    DataDirection direction = DataDirection::Input;
    uint8_t* dataPtr = nullptr;    // pointer into the live process image
};

// A slave discovered on the bus, with its live-mapped PDO variables.
struct DiscoveredSlave {
    uint16_t ringCsa = 0;          // 1-based ring position
    uint16_t reportedCsa = 0;      // backend-reported station address
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
    virtual void configure(const Config& cfg) = 0;

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

    // Performs one full send+receive cycle of process data.
    virtual void updateIO() = 0;

    // Returns all slaves to a safe state and releases the master.
    virtual void shutdown() = 0;
};

// Implemented exactly once, by whichever backend .cpp is compiled in.
std::unique_ptr<IEtherCatBackend> createBackend();

} // namespace ecmqtt
