#include "ecmqtt/esi_repository.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <tinyxml2.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace ecmqtt {

const EsiPdoEntry* EsiDevice::findEntry(uint16_t index, uint8_t subIndex) const {
    for (const auto& pdo : pdos)
        for (const auto& entry : pdo.entries)
            if (entry.index == index && entry.subIndex == subIndex)
                return &entry;
    return nullptr;
}

uint32_t ParseEsiNumber(const std::string& sIn) {
    std::string s = sIn;
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return 0;
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
    if (s.empty()) return 0;

    try {
        if (s.size() > 2 && s[0] == '#' && (s[1] == 'x' || s[1] == 'X'))
            return static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16));
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            return static_cast<uint32_t>(std::stoul(s.substr(2), nullptr, 16));
        return static_cast<uint32_t>(std::stoul(s, nullptr, 10));
    } catch (const std::exception&) {
        return 0;
    }
}

namespace {

// CRC-32 (IEEE 802.3) over a file's raw bytes -- used purely for local
// change detection (has this file changed since the index was written?),
// not as a security hash, so a lightweight, dependency-free checksum is
// enough; no need to pull in a real MD5/SHA implementation for that.
uint32_t Crc32(const std::string& path) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;

    uint32_t crc = 0xFFFFFFFFu;
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        auto n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i)
            crc = table[(crc ^ static_cast<uint8_t>(buf[i])) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void ParsePdoBlock(tinyxml2::XMLElement* deviceEl, const char* tag, DataDirection dir,
                    std::vector<EsiPdo>& out) {
    for (auto* pdoEl = deviceEl->FirstChildElement(tag); pdoEl; pdoEl = pdoEl->NextSiblingElement(tag)) {
        EsiPdo pdo;
        pdo.direction = dir;
        if (auto* idxEl = pdoEl->FirstChildElement("Index"))
            if (const char* txt = idxEl->GetText())
                pdo.index = static_cast<uint16_t>(ParseEsiNumber(txt));

        for (auto* entryEl = pdoEl->FirstChildElement("Entry"); entryEl;
             entryEl = entryEl->NextSiblingElement("Entry")) {
            EsiPdoEntry entry;
            if (auto* e = entryEl->FirstChildElement("Index"))
                if (const char* txt = e->GetText()) entry.index = static_cast<uint16_t>(ParseEsiNumber(txt));
            if (auto* e = entryEl->FirstChildElement("SubIndex"))
                if (const char* txt = e->GetText()) entry.subIndex = static_cast<uint8_t>(ParseEsiNumber(txt));
            if (auto* e = entryEl->FirstChildElement("BitLen"))
                if (const char* txt = e->GetText()) entry.bitLen = static_cast<uint16_t>(ParseEsiNumber(txt));
            if (auto* e = entryEl->FirstChildElement("Name"))
                if (const char* txt = e->GetText()) entry.name = txt;
            if (auto* e = entryEl->FirstChildElement("DataType")) {
                if (const char* txt = e->GetText()) {
                    entry.dataTypeStr = txt;
                    entry.dataType = EsiDataTypeFromString(txt);
                }
            }

            // Zero-length entries are gap/padding placeholders, not real variables.
            if (entry.bitLen > 0)
                pdo.entries.push_back(std::move(entry));
        }
        out.push_back(std::move(pdo));
    }
}

} // namespace

std::vector<EsiRepository::Key> EsiRepository::ExtractDeviceKeys(const std::string& path) {
    std::vector<Key> keys;

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        spdlog::warn("Failed to parse ESI file {}: {}", path, doc.ErrorStr() ? doc.ErrorStr() : "unknown error");
        return keys;
    }

    auto* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) return keys;

    uint32_t vendorId = 0;
    if (auto* vendor = root->FirstChildElement("Vendor"))
        if (auto* id = vendor->FirstChildElement("Id"))
            if (const char* txt = id->GetText())
                vendorId = ParseEsiNumber(txt);

    auto* descriptions = root->FirstChildElement("Descriptions");
    if (!descriptions) return keys;
    auto* devicesEl = descriptions->FirstChildElement("Devices");
    if (!devicesEl) return keys;

    for (auto* deviceEl = devicesEl->FirstChildElement("Device"); deviceEl;
         deviceEl = deviceEl->NextSiblingElement("Device")) {
        auto* typeEl = deviceEl->FirstChildElement("Type");
        if (!typeEl) continue;

        uint32_t productCode = 0, revisionNo = 0;
        if (const char* pc = typeEl->Attribute("ProductCode")) productCode = ParseEsiNumber(pc);
        if (const char* rn = typeEl->Attribute("RevisionNo")) revisionNo = ParseEsiNumber(rn);
        keys.push_back(Key{vendorId, productCode, revisionNo});
    }
    return keys;
}

bool EsiRepository::ParseDeviceFromFile(const std::string& path, const Key& target, EsiDevice& out) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        spdlog::warn("Failed to parse ESI file {}: {}", path, doc.ErrorStr() ? doc.ErrorStr() : "unknown error");
        return false;
    }

    auto* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) return false;

    uint32_t vendorId = 0;
    if (auto* vendor = root->FirstChildElement("Vendor"))
        if (auto* id = vendor->FirstChildElement("Id"))
            if (const char* txt = id->GetText())
                vendorId = ParseEsiNumber(txt);
    if (vendorId != target.vendorId) return false;

    auto* descriptions = root->FirstChildElement("Descriptions");
    if (!descriptions) return false;
    auto* devicesEl = descriptions->FirstChildElement("Devices");
    if (!devicesEl) return false;

    for (auto* deviceEl = devicesEl->FirstChildElement("Device"); deviceEl;
         deviceEl = deviceEl->NextSiblingElement("Device")) {
        auto* typeEl = deviceEl->FirstChildElement("Type");
        if (!typeEl) continue;

        uint32_t productCode = 0, revisionNo = 0;
        if (const char* pc = typeEl->Attribute("ProductCode")) productCode = ParseEsiNumber(pc);
        if (const char* rn = typeEl->Attribute("RevisionNo")) revisionNo = ParseEsiNumber(rn);
        if (productCode != target.productCode || revisionNo != target.revisionNo) continue;

        out.vendorId = vendorId;
        out.productCode = productCode;
        out.revisionNo = revisionNo;
        if (const char* typeText = typeEl->GetText()) out.name = typeText;

        if (auto* nameEl = deviceEl->FirstChildElement("Name"))
            if (const char* txt = nameEl->GetText())
                out.name = txt;

        if (auto* commentEl = deviceEl->FirstChildElement("Comment"))
            if (const char* txt = commentEl->GetText())
                out.description = txt;
        if (out.description.empty())
            out.description = out.name;

        ParsePdoBlock(deviceEl, "RxPdo", DataDirection::Output, out.pdos);
        ParsePdoBlock(deviceEl, "TxPdo", DataDirection::Input, out.pdos);
        return true;
    }
    return false;
}

void EsiRepository::refreshIndex(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        spdlog::warn("ESI directory {} does not exist; slave metadata will be numeric-only", dir);
        return;
    }

    auto startTime = std::chrono::steady_clock::now();
    std::unordered_map<std::string, IndexedFile> refreshed;
    size_t reused = 0, reparsed = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".xml") continue;

        std::string path = entry.path().string();
        std::string hash = std::to_string(Crc32(path));

        auto it = fileIndex_.find(path);
        if (it != fileIndex_.end() && it->second.hash == hash) {
            refreshed.emplace(path, std::move(it->second));
            ++reused;
            continue;
        }

        IndexedFile idx;
        idx.hash = hash;
        idx.deviceKeys = ExtractDeviceKeys(path);
        refreshed.emplace(path, std::move(idx));
        ++reparsed;
    }

    fileIndex_ = std::move(refreshed);

    deviceFile_.clear();
    for (auto& [path, idx] : fileIndex_)
        for (auto& key : idx.deviceKeys) deviceFile_.emplace(key, path);

    auto totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
    spdlog::info(
        "ESI index: {} device(s) across {} file(s) in {} ({} file(s) unchanged, {} (re)scanned, {:.0f}ms)",
        deviceFile_.size(), fileIndex_.size(), dir, reused, reparsed, totalMs);
}

void EsiRepository::loadIndex(const std::string& path) {
    std::ifstream f(path);
    if (!f) return; // no index yet -- nothing to do

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& ex) {
        spdlog::warn("Failed to parse ESI index {}: {}", path, ex.what());
        return;
    }

    fileIndex_.clear();
    // items() wraps a reference to the object it's called on -- binding the
    // "files" object to a named local first (rather than calling .items()
    // straight off the .value(...) temporary) keeps it alive for the loop;
    // nlohmann::json::items() on a temporary is a dangling-reference trap.
    nlohmann::json files = j.value("files", nlohmann::json::object());
    for (auto& [filePath, fj] : files.items()) {
        IndexedFile idx;
        idx.hash = fj.value("hash", "");
        for (auto& dj : fj.value("devices", nlohmann::json::array()))
            idx.deviceKeys.push_back(
                {dj.value("vendorId", 0u), dj.value("productCode", 0u), dj.value("revisionNo", 0u)});
        fileIndex_.emplace(filePath, std::move(idx));
    }

    deviceFile_.clear();
    for (auto& [filePath, idx] : fileIndex_)
        for (auto& key : idx.deviceKeys) deviceFile_.emplace(key, filePath);

    spdlog::info("ESI index: loaded {} known device(s) across {} file(s) from {}", deviceFile_.size(),
                 fileIndex_.size(), path);
}

void EsiRepository::saveIndex(const std::string& path) const {
    std::error_code ec;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    nlohmann::json filesJson = nlohmann::json::object();
    for (auto& [filePath, idx] : fileIndex_) {
        nlohmann::json devicesJson = nlohmann::json::array();
        for (auto& key : idx.deviceKeys)
            devicesJson.push_back(
                {{"vendorId", key.vendorId}, {"productCode", key.productCode}, {"revisionNo", key.revisionNo}});
        filesJson[filePath] = {{"hash", idx.hash}, {"devices", devicesJson}};
    }

    nlohmann::json j;
    j["files"] = filesJson;

    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        spdlog::warn("Failed to write ESI index {}", path);
        return;
    }
    f << j.dump(2);
}

const EsiDevice* EsiRepository::resolve(uint32_t vendorId, uint32_t productCode, uint32_t revisionNo) {
    Key key{vendorId, productCode, revisionNo};

    auto cached = devices_.find(key);
    if (cached != devices_.end()) return &cached->second;

    auto fileIt = deviceFile_.find(key);
    if (fileIt == deviceFile_.end()) return nullptr;

    EsiDevice dev;
    if (!ParseDeviceFromFile(fileIt->second, key, dev)) {
        spdlog::warn(
            "ESI index pointed at {} for vendor {:#x} product {:#x} rev {:#x}, but it wasn't found there "
            "(file changed on disk since the index was last refreshed?)",
            fileIt->second, vendorId, productCode, revisionNo);
        return nullptr;
    }

    auto [it, inserted] = devices_.emplace(key, std::move(dev));
    (void)inserted;
    return &it->second;
}

} // namespace ecmqtt
