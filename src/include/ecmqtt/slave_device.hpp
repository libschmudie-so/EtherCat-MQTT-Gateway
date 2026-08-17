#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ecmqtt/ethercat_backend.hpp"

namespace ecmqtt {

// One ESI-declared RxPdo/TxPdo option for a device -- not necessarily the
// one currently assigned. Surfaced in metadata so a client (e.g. a GUI) can
// offer a picker for --pdo-config's rxPdo/txPdo selectors without needing
// its own copy of ESI.
struct AvailablePdo {
    uint16_t index = 0;
    std::string name; // may be empty if ESI doesn't declare one for this PDO
};

// Wraps a discovered EtherCAT slave and exposes its PDO variables as typed
// JSON read/write helpers. Bit-level packing mirrors the EtherCAT.NET-based
// original: values are always treated as little-endian, LSB-first bit
// streams starting at SlaveVariable::bitOffset within SlaveVariable::dataPtr.
class SlaveDevice {
public:
    SlaveDevice(DiscoveredSlave& slave, std::string name, std::string description,
                std::vector<AvailablePdo> availableRxPdos = {}, std::vector<AvailablePdo> availableTxPdos = {});

    const std::string& GetName() const { return name_; }
    uint16_t GetCsa(bool useReportedCsa = false) const {
        return useReportedCsa ? slave_->reportedCsa : slave_->ringCsa;
    }
    uint16_t GetReportedCsa() const { return slave_->reportedCsa; }

    nlohmann::json GetMetadata() const;

    std::vector<SlaveVariable*> GetAllVariables();
    std::vector<SlaveVariable*> GetInputVariables();
    std::vector<SlaveVariable*> GetOutputVariables();

    nlohmann::json ReadVariableAsJson(const SlaveVariable& variable) const;
    void WriteVariableAsJson(const SlaveVariable& variable, const nlohmann::json& value);

private:
    DiscoveredSlave* slave_;
    std::string name_;
    std::string description_;
    std::vector<AvailablePdo> availableRxPdos_;
    std::vector<AvailablePdo> availableTxPdos_;
};

} // namespace ecmqtt
