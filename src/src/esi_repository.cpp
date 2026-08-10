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

nlohmann::json SerializeDevice(const EsiDevice& d) {
    nlohmann::json pdos = nlohmann::json::array();
    for (const auto& pdo : d.pdos) {
        nlohmann::json entries = nlohmann::json::array();
        for (const auto& e : pdo.entries) {
            entries.push_back({
                {"index", e.index},
                {"subIndex", e.subIndex},
                {"bitLen", e.bitLen},
                {"name", e.name},
                {"dataTypeStr", e.dataTypeStr},
            });
        }
        pdos.push_back({
            {"index", pdo.index},
            {"direction", pdo.direction == DataDirection::Output ? "Output" : "Input"},
            {"entries", entries},
        });
    }
    return {
        {"vendorId", d.vendorId}, {"productCode", d.productCode}, {"revisionNo", d.revisionNo},
        {"name", d.name},         {"description", d.description}, {"pdos", pdos},
    };
}

std::optional<EsiDevice> DeserializeDevice(const nlohmann::json& j) {
    try {
        EsiDevice d;
        d.vendorId = j.at("vendorId").get<uint32_t>();
        d.productCode = j.at("productCode").get<uint32_t>();
        d.revisionNo = j.at("revisionNo").get<uint32_t>();
        d.name = j.value("name", "");
        d.description = j.value("description", "");

        for (const auto& pj : j.value("pdos", nlohmann::json::array())) {
            EsiPdo pdo;
            pdo.index = pj.at("index").get<uint16_t>();
            pdo.direction = pj.value("direction", "Input") == "Output" ? DataDirection::Output : DataDirection::Input;

            for (const auto& ej : pj.value("entries", nlohmann::json::array())) {
                EsiPdoEntry entry;
                entry.index = ej.at("index").get<uint16_t>();
                entry.subIndex = ej.at("subIndex").get<uint8_t>();
                entry.bitLen = ej.at("bitLen").get<uint16_t>();
                entry.name = ej.value("name", "");
                entry.dataTypeStr = ej.value("dataTypeStr", "");
                entry.dataType = EsiDataTypeFromString(entry.dataTypeStr);
                pdo.entries.push_back(std::move(entry));
            }
            d.pdos.push_back(std::move(pdo));
        }
        return d;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

} // namespace

bool EsiRepository::loadFile(const std::string& path, const EsiFilter* filter) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        spdlog::warn("Failed to parse ESI file {}: {}", path, doc.ErrorStr() ? doc.ErrorStr() : "unknown error");
        return false;
    }

    auto* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) return true;

    uint32_t vendorId = 0;
    if (auto* vendor = root->FirstChildElement("Vendor"))
        if (auto* id = vendor->FirstChildElement("Id"))
            if (const char* txt = id->GetText())
                vendorId = ParseEsiNumber(txt);

    bool filterDevices = filter && !filter->deviceKeys.empty();

    auto* descriptions = root->FirstChildElement("Descriptions");
    if (!descriptions) return true;
    auto* devicesEl = descriptions->FirstChildElement("Devices");
    if (!devicesEl) return true;

    size_t countBefore = devices_.size();

    for (auto* deviceEl = devicesEl->FirstChildElement("Device"); deviceEl;
         deviceEl = deviceEl->NextSiblingElement("Device")) {
        auto* typeEl = deviceEl->FirstChildElement("Type");
        if (!typeEl) continue;

        uint32_t productCode = 0, revisionNo = 0;
        if (const char* pc = typeEl->Attribute("ProductCode")) productCode = ParseEsiNumber(pc);
        if (const char* rn = typeEl->Attribute("RevisionNo")) revisionNo = ParseEsiNumber(rn);

        // Skip the (potentially large) name/description/PDO extraction for
        // devices nothing on the bus actually is -- find() would just never
        // be asked about them anyway.
        if (filterDevices &&
            filter->deviceKeys.find(EsiFilter::MakeDeviceKey(productCode, revisionNo)) == filter->deviceKeys.end())
            continue;

        EsiDevice device;
        device.vendorId = vendorId;
        device.productCode = productCode;
        device.revisionNo = revisionNo;
        if (const char* typeText = typeEl->GetText()) device.name = typeText;

        if (auto* nameEl = deviceEl->FirstChildElement("Name"))
            if (const char* txt = nameEl->GetText())
                device.name = txt;

        if (auto* commentEl = deviceEl->FirstChildElement("Comment"))
            if (const char* txt = commentEl->GetText())
                device.description = txt;
        if (device.description.empty())
            device.description = device.name;

        ParsePdoBlock(deviceEl, "RxPdo", DataDirection::Output, device.pdos);
        ParsePdoBlock(deviceEl, "TxPdo", DataDirection::Input, device.pdos);

        Key key{device.vendorId, device.productCode, device.revisionNo};
        devices_[key] = std::move(device);
    }

    spdlog::debug("Loaded {} device(s) from ESI file {}", devices_.size() - countBefore, path);
    return true;
}

void EsiRepository::loadDirectory(const std::string& dir, const EsiFilter* filter) {
    if (filter && filter->vendorIds.empty() && filter->deviceKeys.empty())
        filter = nullptr; // degenerate empty filter means "no filtering"

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        spdlog::warn("ESI directory {} does not exist; slave metadata will be numeric-only", dir);
        return;
    }

    auto startTime = std::chrono::steady_clock::now();
    size_t filesParsed = 0, filesFailed = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".xml") continue;

        auto path = entry.path().string();

        auto fileStart = std::chrono::steady_clock::now();
        bool ok = loadFile(path, filter);
        auto fileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fileStart).count();

        if (ok) {
            if (fileMs > 500.0) spdlog::info("Parsed ESI file {} in {:.0f}ms", path, fileMs);
            ++filesParsed;
        } else {
            ++filesFailed;
        }
    }

    auto totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
    spdlog::info("ESI repository loaded {} device descriptor(s) from {} ({} file(s) parsed, {} failed, {:.0f}ms)",
                 devices_.size(), dir, filesParsed, filesFailed, totalMs);
}

void EsiRepository::loadCacheFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return; // no cache yet -- nothing to do

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& ex) {
        spdlog::warn("Failed to parse ESI cache {}: {}", path, ex.what());
        return;
    }

    size_t loaded = 0;
    for (const auto& dj : j.value("devices", nlohmann::json::array())) {
        auto dev = DeserializeDevice(dj);
        if (!dev) continue;
        Key key{dev->vendorId, dev->productCode, dev->revisionNo};
        devices_[key] = std::move(*dev);
        ++loaded;
    }

    spdlog::info("ESI cache: loaded {} known device(s) from {}", loaded, path);
}

void EsiRepository::saveCacheFile(const std::string& path) const {
    std::error_code ec;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    nlohmann::json j;
    j["devices"] = nlohmann::json::array();
    for (const auto& [key, device] : devices_)
        j["devices"].push_back(SerializeDevice(device));

    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        spdlog::warn("Failed to write ESI cache {}", path);
        return;
    }
    f << j.dump(2);
}

const EsiDevice* EsiRepository::find(uint32_t vendorId, uint32_t productCode, uint32_t revisionNo) const {
    Key key{vendorId, productCode, revisionNo};
    auto it = devices_.find(key);
    return it == devices_.end() ? nullptr : &it->second;
}

} // namespace ecmqtt
