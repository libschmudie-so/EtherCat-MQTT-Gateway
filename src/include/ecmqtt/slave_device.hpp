#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ecmqtt/ethercat_backend.hpp"

namespace ecmqtt {

// Wraps a discovered EtherCAT slave and exposes its PDO variables as typed
// JSON read/write helpers. Bit-level packing mirrors the EtherCAT.NET-based
// original: values are always treated as little-endian, LSB-first bit
// streams starting at SlaveVariable::bitOffset within SlaveVariable::dataPtr.
class SlaveDevice {
public:
    SlaveDevice(DiscoveredSlave& slave, std::string name, std::string description);

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
};

} // namespace ecmqtt
