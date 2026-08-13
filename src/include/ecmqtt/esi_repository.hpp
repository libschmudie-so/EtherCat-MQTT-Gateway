#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ecmqtt/ethercat_types.hpp"

namespace ecmqtt {

struct EsiPdoEntry {
    uint16_t index = 0;
    uint8_t subIndex = 0;
    uint16_t bitLen = 0;
    std::string name;
    // From the entry's own <Comment>, if present; falls back to name (same
    // pattern as EsiDevice::description) so consumers always get something
    // readable rather than having to fall back themselves.
    std::string description;
    std::string dataTypeStr;
    EthercatDataType dataType = EthercatDataType::Unknown;
};

struct EsiPdo {
    uint16_t index = 0;
    DataDirection direction = DataDirection::Input; // RxPdo -> Output, TxPdo -> Input
    std::vector<EsiPdoEntry> entries;
};

struct EsiDevice {
    uint32_t vendorId = 0;
    uint32_t productCode = 0;
    uint32_t revisionNo = 0;
    std::string name;
    std::string description;
    std::vector<EsiPdo> pdos;

    // Finds a PDO entry by (index, subIndex) across all Rx/Tx PDOs.
    const EsiPdoEntry* findEntry(uint16_t index, uint8_t subIndex) const;
};

// Indexes ESI *.xml files by which device (VendorId, ProductCode,
// RevisionNo) each one describes, without duplicating their content: the
// persisted index (see loadIndex()/saveIndex()) only stores, per file, a
// change-detection hash and the list of devices it declares. Full device
// detail (name, PDOs) is parsed from the actual file on demand, one file at
// a time, the first time it's actually needed -- see resolve(). This trades
// a small per-run parse cost (one targeted file per distinct device
// actually in use, not the whole ESI directory) for never going stale
// relative to the ESI files themselves and for a much smaller cache file.
class EsiRepository {
public:
    // Loads a previously-saved index (see saveIndex()); silently does
    // nothing if the file doesn't exist yet.
    void loadIndex(const std::string& path);

    // Rescans dir for *.xml files, keeping the index in sync: a file whose
    // content hash still matches what's already recorded is trusted as-is
    // (not re-parsed at all); a new or changed file gets a lightweight scan
    // (device identity only -- no name/PDO detail, that's resolved lazily
    // by resolve()) to (re)populate its entry. Entries for files no longer
    // present under dir are dropped. Call this once at startup, before any
    // resolve() calls.
    void refreshIndex(const std::string& dir);

    // Writes the current index to path. Creates parent directories as
    // needed.
    void saveIndex(const std::string& path) const;

    // Resolves (vendorId, productCode, revisionNo) to full device detail.
    // Returns from an in-memory cache if already resolved earlier this run;
    // otherwise looks up which file the index says declares it and parses
    // just that one file, on demand. Returns nullptr if the device isn't in
    // the index at all (refreshIndex() wasn't pointed at the right
    // directory, or no ESI file declares it).
    const EsiDevice* resolve(uint32_t vendorId, uint32_t productCode, uint32_t revisionNo);

    // Total distinct devices known to the index, whether or not their full
    // detail has been resolved into memory yet this run.
    size_t indexedDeviceCount() const { return deviceFile_.size(); }

private:
    struct Key {
        uint32_t vendorId, productCode, revisionNo;
        bool operator==(const Key& o) const {
            return vendorId == o.vendorId && productCode == o.productCode && revisionNo == o.revisionNo;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<uint32_t>()(k.vendorId);
            h ^= std::hash<uint32_t>()(k.productCode) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>()(k.revisionNo) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct IndexedFile {
        std::string hash;
        std::vector<Key> deviceKeys;
    };

    // Lightweight scan: just the device identities declared in a file, no
    // name/PDO detail -- used to (re)build the index cheaply.
    static std::vector<Key> ExtractDeviceKeys(const std::string& path);
    // Full targeted parse: extracts name/description/PDOs for exactly one
    // device out of a (possibly much larger, multi-device) ESI file.
    static bool ParseDeviceFromFile(const std::string& path, const Key& target, EsiDevice& out);

    std::unordered_map<std::string, IndexedFile> fileIndex_;   // file path -> hash + devices it declares
    std::unordered_map<Key, std::string, KeyHash> deviceFile_; // device -> owning file path (derived from fileIndex_)
    std::unordered_map<Key, EsiDevice, KeyHash> devices_;      // resolved so far this run
};

// Parses an ESI-style number: "#x1234" (hex), "0x1234" (hex), or a plain
// decimal string. Returns 0 if the string is empty/unparseable.
uint32_t ParseEsiNumber(const std::string& s);

} // namespace ecmqtt
