#include "ecmqtt/pdo_override.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ecmqtt/esi_repository.hpp" // ParseEsiNumber

namespace ecmqtt {
namespace {

uint32_t NumFromJson(const nlohmann::json& j) {
    if (j.is_string()) return ParseEsiNumber(j.get<std::string>());
    return j.get<uint32_t>();
}

} // namespace

std::vector<PdoOverride> LoadPdoOverrideConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open --pdo-config file: " + path);

    nlohmann::json j;
    f >> j; // nlohmann::json::parse_error propagates with a useful message

    std::vector<PdoOverride> result;
    for (const auto& oj : j.value("overrides", nlohmann::json::array())) {
        PdoOverride ov;
        ov.vendorId = NumFromJson(oj.at("vendorId"));
        ov.productCode = NumFromJson(oj.at("productCode"));
        ov.revisionNo = NumFromJson(oj.at("revisionNo"));
        for (const auto& e : oj.value("rxPdo", nlohmann::json::array()))
            ov.rxPdoIndices.push_back(static_cast<uint16_t>(NumFromJson(e)));
        for (const auto& e : oj.value("txPdo", nlohmann::json::array()))
            ov.txPdoIndices.push_back(static_cast<uint16_t>(NumFromJson(e)));
        result.push_back(std::move(ov));
    }
    return result;
}

} // namespace ecmqtt
