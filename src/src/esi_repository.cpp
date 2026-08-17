#include "ecmqtt/esi_repository.hpp"

#include <algorithm>
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

void ParsePdoBlock(tinyxml2::XMLElement* deviceEl, const char* tag, DataDirection dir,
                    std::vector<EsiPdo>& out) {
    for (auto* pdoEl = deviceEl->FirstChildElement(tag); pdoEl; pdoEl = pdoEl->NextSiblingElement(tag)) {
        EsiPdo pdo;
        pdo.direction = dir;
        if (auto* idxEl = pdoEl->FirstChildElement("Index"))
            if (const char* txt = idxEl->GetText())
                pdo.index = static_cast<uint16_t>(ParseEsiNumber(txt));
        if (auto* nameEl = pdoEl->FirstChildElement("Name"))
            if (const char* txt = nameEl->GetText())
                pdo.name = txt;

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
            if (auto* e = entryEl->FirstChildElement("Comment"))
                if (const char* txt = e->GetText()) entry.description = txt;
            if (entry.description.empty()) entry.description = entry.name;
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
        // name is the short <Type> text (e.g. "EL1008") -- the identifier
        // printed on the terminal itself. <Name> is typically that same
        // text followed by a longer description (e.g. "EL1008 8Ch. Dig.
        // Input 24V, 3ms"); keep the two separate rather than letting the
        // longer one clobber the short one, matching how the original
        // (pre-port) tool treated them.
        if (const char* typeText = typeEl->GetText()) out.name = typeText;

        std::string longName;
        if (auto* nameEl = deviceEl->FirstChildElement("Name"))
            if (const char* txt = nameEl->GetText())
                longName = txt;

        if (auto* commentEl = deviceEl->FirstChildElement("Comment"))
            if (const char* txt = commentEl->GetText())
                out.description = txt;
        if (out.description.empty()) {
            // <Name> commonly repeats <Type> verbatim as its own prefix --
            // strip that duplicate so the description adds information
            // instead of just restating the name.
            out.description = longName;
            if (!out.name.empty() && out.description.rfind(out.name, 0) == 0) {
                out.description = out.description.substr(out.name.size());
                size_t start = out.description.find_first_not_of(" \t");
                out.description = start == std::string::npos ? "" : out.description.substr(start);
            }
        }
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

        // A size+mtime match is trusted as "unchanged" without reading the
        // file at all -- directory_iterator's entry already has these
        // cached from the readdir/stat it just did.
        std::error_code statEc;
        uintmax_t size = entry.file_size(statEc);
        int64_t mtime = statEc ? 0 : entry.last_write_time(statEc).time_since_epoch().count();

        auto it = fileIndex_.find(path);
        if (!statEc && it != fileIndex_.end() && it->second.size == size && it->second.mtime == mtime) {
            refreshed.emplace(path, std::move(it->second));
            ++reused;
            continue;
        }

        auto fileStart = std::chrono::steady_clock::now();
        IndexedFile idx;
        idx.size = size;
        idx.mtime = mtime;
        idx.deviceKeys = ExtractDeviceKeys(path);
        auto fileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fileStart).count();
        spdlog::info("ESI index: loaded {} ({:.0f}ms)", entry.path().filename().string(), fileMs);
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
        idx.size = fj.value("size", uintmax_t{0});
        idx.mtime = fj.value("mtime", int64_t{0});
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
        filesJson[filePath] = {{"size", idx.size}, {"mtime", idx.mtime}, {"devices", devicesJson}};
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
