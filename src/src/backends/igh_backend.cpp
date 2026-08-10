// IGH (Etherlab) master backend. Uses the live introspection API
// (ecrt_master_get_slave/_sync_manager/_pdo/_pdo_entry) to read each slave's
// currently-assigned PDO mapping (its SII/EEPROM default, same source SOEM's
// default config uses), then registers each entry by position
// (ecrt_slave_config_reg_pdo_entry_pos) to obtain a byte offset into a
// single shared domain. Verified against the IGH 1.6.8 ecrt.h found on this
// machine; re-check struct/field names against your installed header if a
// different IGH version renames anything here.
//
// ec_pdo_info_t/ec_pdo_entry_info_t carry no name field -- IGH's live
// introspection, like SOEM's, has no human-readable PDO names. That's why
// ESI-file metadata is required for both backends, not just SOEM.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <ecrt.h>

#include <spdlog/spdlog.h>

#include "ecmqtt/ethercat_backend.hpp"

namespace ecmqtt {
namespace {

class IghBackend final : public IEtherCatBackend {
public:
    ~IghBackend() override {
        if (master_) ecrt_release_master(master_);
    }

    void configure(const Config& cfg) override {
        (void)cfg; // IGH addresses slaves via its own preconfigured master/device, not an --iface name

        master_ = ecrt_request_master(0);
        if (!master_)
            throw std::runtime_error(
                "ecrt_request_master(0) failed -- is the ethercat master service running and device configured?");

        domain_ = ecrt_master_create_domain(master_);
        if (!domain_) throw std::runtime_error("ecrt_master_create_domain failed");

        ec_master_info_t info{};
        if (ecrt_master(master_, &info) != 0) throw std::runtime_error("ecrt_master() failed to obtain master info");

        slaves_.reserve(info.slave_count);

        for (unsigned int pos = 0; pos < info.slave_count; ++pos) {
            ec_slave_info_t slaveInfo{};
            if (ecrt_master_get_slave(master_, static_cast<uint16_t>(pos), &slaveInfo) != 0) {
                spdlog::warn("IGH: failed to get slave info at position {}", pos);
                continue;
            }

            DiscoveredSlave ds;
            ds.ringCsa = static_cast<uint16_t>(pos + 1); // keep 1-based ring numbering, consistent with SOEM
            ds.reportedCsa = ds.ringCsa; // IGH exposes no separate configured-station-address concept
            ds.vendorId = slaveInfo.vendor_id;
            ds.productCode = slaveInfo.product_code;
            ds.revisionNo = slaveInfo.revision_number;
            ds.liveName = slaveInfo.name;

            ec_slave_config_t* sc = ecrt_master_slave_config(master_, /*alias=*/0, static_cast<uint16_t>(pos),
                                                              slaveInfo.vendor_id, slaveInfo.product_code);
            if (!sc) {
                spdlog::warn("IGH: ecrt_master_slave_config failed for slave {} ({})", pos, ds.liveName);
                slaves_.push_back(std::move(ds));
                continue;
            }

            for (uint8_t syncIdx = 0; syncIdx < slaveInfo.sync_count; ++syncIdx) {
                ec_sync_info_t sync{};
                if (ecrt_master_get_sync_manager(master_, static_cast<uint16_t>(pos), syncIdx, &sync) != 0) continue;
                if (sync.dir != EC_DIR_OUTPUT && sync.dir != EC_DIR_INPUT) continue; // mailbox SMs etc.

                DataDirection dir = (sync.dir == EC_DIR_OUTPUT) ? DataDirection::Output : DataDirection::Input;

                for (unsigned int pdoPos = 0; pdoPos < sync.n_pdos; ++pdoPos) {
                    ec_pdo_info_t pdo{};
                    if (ecrt_master_get_pdo(master_, static_cast<uint16_t>(pos), syncIdx,
                                             static_cast<uint16_t>(pdoPos), &pdo) != 0)
                        continue;

                    for (unsigned int entryPos = 0; entryPos < pdo.n_entries; ++entryPos) {
                        ec_pdo_entry_info_t entry{};
                        if (ecrt_master_get_pdo_entry(master_, static_cast<uint16_t>(pos), syncIdx,
                                                       static_cast<uint16_t>(pdoPos),
                                                       static_cast<uint16_t>(entryPos), &entry) != 0)
                            continue;
                        if (entry.bit_length == 0 || entry.index == 0) continue; // gap/padding entry

                        unsigned int bitPosition = 0;
                        int byteOffset = ecrt_slave_config_reg_pdo_entry_pos(
                            sc, syncIdx, pdoPos, entryPos, domain_, &bitPosition);
                        if (byteOffset < 0) {
                            spdlog::warn("IGH: failed to register PDO entry {:#06x}:{} on slave {}", entry.index,
                                         entry.subindex, pos);
                            continue;
                        }

                        SlaveVariable v;
                        v.index = entry.index;
                        v.subIndex = entry.subindex;
                        v.bitLength = entry.bit_length;
                        v.bitOffset = static_cast<uint8_t>(bitPosition);
                        v.direction = dir;
                        v.dataType = GuessDataTypeFromBitLength(entry.bit_length); // refined via ESI later
                        ds.variables.push_back(v);

                        pending_.push_back({slaves_.size(), ds.variables.size() - 1, byteOffset});
                    }
                }
            }

            slaves_.push_back(std::move(ds));
        }

        spdlog::info("IGH: {} slave(s) discovered and registered", slaves_.size());
    }

    // ecrt_master_activate() is what starts the master expecting cyclic
    // servicing (pending mailbox/SDO exchanges from configure()'s bus scan
    // ride along on the cyclic exchange); call this right before the cyclic
    // loop starts, per the base class contract.
    void activate() override {
        if (ecrt_master_activate(master_) != 0) throw std::runtime_error("ecrt_master_activate failed");

        uint8_t* domainBase = ecrt_domain_data(domain_);
        if (!domainBase) throw std::runtime_error("ecrt_domain_data returned null after activation");

        for (const auto& p : pending_)
            slaves_[p.slaveArrIdx].variables[p.varIdx].dataPtr = domainBase + p.byteOffset;
        pending_.clear();

        spdlog::info("IGH: master activated");
    }

    std::vector<DiscoveredSlave>& slaves() override { return slaves_; }

    void updateIO() override {
        ecrt_master_receive(master_);
        ecrt_domain_process(domain_);
        ecrt_domain_queue(domain_);
        ecrt_master_send(master_);
    }

    void shutdown() override {
        if (master_) ecrt_master_deactivate(master_);
    }

private:
    struct PendingOffset {
        size_t slaveArrIdx;
        size_t varIdx;
        int byteOffset;
    };

    ec_master_t* master_ = nullptr;
    ec_domain_t* domain_ = nullptr;
    std::vector<DiscoveredSlave> slaves_;
    std::vector<PendingOffset> pending_;
};

} // namespace

std::unique_ptr<IEtherCatBackend> createBackend() { return std::make_unique<IghBackend>(); }

} // namespace ecmqtt
