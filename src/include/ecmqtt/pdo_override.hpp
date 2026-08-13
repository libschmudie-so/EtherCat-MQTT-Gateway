#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ecmqtt {

// A single PDO entry to map. For SOEM this is informational only (its live
// CoE readback discovers entries dynamically once the assignment changes).
// For IGH it's required -- ecrt_slave_config_pdos() needs the full entry
// list up front, since by the time configure() returns it's too late to
// change what gets registered.
struct PdoOverrideEntry {
    uint16_t index = 0;
    uint8_t subIndex = 0;
    uint16_t bitLen = 0;
};

struct PdoOverridePdo {
    uint16_t pdoIndex = 0;
    std::vector<PdoOverrideEntry> entries; // resolved from ESI by main.cpp before configure()
};

// Requests a non-default PDO assignment for a specific device, identified by
// (vendorId, productCode, revisionNo) rather than ring position -- so the
// same override still applies if the slave is moved, or swapped for an
// identical replacement unit. Many terminals declare more than one
// RxPdo/TxPdo block in their ESI (e.g. "Standard"/"Compact"/"Enhanced"
// variants); rxPdoIndices/txPdoIndices pick which of those to assign instead
// of whatever's currently active by default.
struct PdoOverride {
    uint32_t vendorId = 0, productCode = 0, revisionNo = 0;
    std::vector<uint16_t> rxPdoIndices; // requested RxPDO (Output) assignment, in order
    std::vector<uint16_t> txPdoIndices; // requested TxPDO (Input) assignment, in order
    std::vector<PdoOverridePdo> rxPdos; // resolved (index + entries); filled in by main.cpp
    std::vector<PdoOverridePdo> txPdos;
};

// Parses a --pdo-config JSON file:
// { "overrides": [ { "vendorId": "#x2", "productCode": "#x...", "revisionNo": "#x...",
//                     "rxPdo": ["#x1600", "#x1601"], "txPdo": ["#x1a02"] } ] }
// Numbers accept ESI-style ("#x..", "0x..") or plain decimal strings, or
// JSON integers. Only rxPdoIndices/txPdoIndices are populated here -- entry
// content is resolved separately against ESI. Throws std::runtime_error /
// nlohmann::json::exception on malformed input.
std::vector<PdoOverride> LoadPdoOverrideConfig(const std::string& path);

} // namespace ecmqtt
