#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ecmqtt/ethercat_types.hpp"

namespace ecmqtt {

struct EsiPdoEntry {
    uint16_t index = 0;
    uint8_t subIndex = 0;
    uint16_t bitLen = 0;
    std::string name;
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

// Restricts loadDirectory() to only the vendors/devices actually present on
// the bus. Real-world ESI directories (e.g. a full vendor's official XML
// dump) can hold many megabytes and thousands of unrelated device
// descriptions across many files; without filtering, loadDirectory() parses
// all of it up front regardless of what's connected. Both sets should come
// from the same discovered-slave list; an empty filter (default-constructed,
// or passed as nullptr to loadDirectory) means "load everything".
struct EsiFilter {
    std::unordered_set<uint32_t> vendorIds;
    std::unordered_set<uint64_t> deviceKeys; // MakeDeviceKey(productCode, revisionNo)

    static uint64_t MakeDeviceKey(uint32_t productCode, uint32_t revisionNo) {
        return (static_cast<uint64_t>(productCode) << 32) | revisionNo;
    }
};

// Loads and indexes every ESI *.xml file in a directory, keyed by
// (VendorId, ProductCode, RevisionNo).
class EsiRepository {
public:
    // Loads all *.xml files directly inside dir. Parse errors on individual
    // files are logged and skipped rather than aborting the whole load. If
    // filter is non-null and non-empty, devices not present in it are
    // skipped while walking each file's DOM tree (their name/PDO details
    // are never extracted) -- pass nullptr to load every device found.
    void loadDirectory(const std::string& dir, const EsiFilter* filter = nullptr);

    // Loads devices previously written by saveCacheFile(), if the file
    // exists (silently does nothing otherwise). Call this before
    // loadDirectory() so already-known devices don't need to be re-parsed.
    void loadCacheFile(const std::string& path);

    // Writes every currently-known device to path as JSON, so a future run
    // with the same hardware can skip re-parsing the ESI directory for them
    // entirely via loadCacheFile(). Creates parent directories as needed.
    void saveCacheFile(const std::string& path) const;

    const EsiDevice* find(uint32_t vendorId, uint32_t productCode, uint32_t revisionNo) const;

    size_t deviceCount() const { return devices_.size(); }

private:
    // Returns true if the file was actually DOM-parsed, false if it failed
    // to load/parse.
    bool loadFile(const std::string& path, const EsiFilter* filter);

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

    std::unordered_map<Key, EsiDevice, KeyHash> devices_;
};

// Parses an ESI-style number: "#x1234" (hex), "0x1234" (hex), or a plain
// decimal string. Returns 0 if the string is empty/unparseable.
uint32_t ParseEsiNumber(const std::string& s);

} // namespace ecmqtt
