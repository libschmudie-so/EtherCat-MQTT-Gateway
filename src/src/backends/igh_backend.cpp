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

#include "ecmqtt/esi_repository.hpp"
#include "ecmqtt/ethercat_backend.hpp"
#include "ecmqtt/pdo_override.hpp"

namespace ecmqtt {
namespace {

const PdoOverride* FindOverride(const std::vector<PdoOverride>& overrides, uint32_t vendorId, uint32_t productCode,
                                 uint32_t revisionNo) {
    for (auto& ov : overrides)
        if (ov.vendorId == vendorId && ov.productCode == productCode && ov.revisionNo == revisionNo) return &ov;
    return nullptr;
}

class IghBackend final : public IEtherCatBackend {
public:
    ~IghBackend() override {
        if (master_) ecrt_release_master(master_);
    }

    void configure(const Config& cfg, EsiRepository& esiRepo) override {
        // cfg.interface is unused: IGH addresses slaves via its own
        // preconfigured master/device, not an --iface name. cfg.pdoOverrides
        // is used below.
        master_ = ecrt_request_master(0);
        if (!master_)
            throw std::runtime_error(
                "ecrt_request_master(0) failed -- is the ethercat master service running and device configured?");

        domain_ = ecrt_master_create_domain(master_);
        if (!domain_) throw std::runtime_error("ecrt_master_create_domain failed");

        rescan(cfg, esiRepo);
    }

    // ecrt_master_activate() is what starts the master expecting cyclic
    // servicing (pending mailbox/SDO exchanges from configure()'s bus scan
    // ride along on the cyclic exchange); call this right before the cyclic
    // loop starts, per the base class contract.
    void activate() override {
        if (ecrt_master_activate(master_) != 0) throw std::runtime_error("ecrt_master_activate failed");

        uint8_t* domainBase = ecrt_domain_data(domain_);
        // ecrt_domain_data() legitimately returns NULL for a zero-size
        // domain (no PDO entries registered at all -- e.g. every slave was
        // unplugged, or the ones present expose no process data), not just
        // on a real failure. Only treat it as fatal when we actually have
        // offsets waiting to be resolved against it; otherwise pending_ is
        // empty and the loop below is a no-op anyway.
        if (!domainBase && !pending_.empty())
            throw std::runtime_error("ecrt_domain_data returned null after activation");

        for (const auto& p : pending_)
            slaves_[p.slaveArrIdx].variables[p.varIdx].dataPtr = domainBase + p.byteOffset;
        pending_.clear();

        spdlog::info("IGH: master activated");
    }

    std::vector<DiscoveredSlave>& slaves() override { return slaves_; }

    // IGH's documented cyclic pattern is receive -> process -> read inputs
    // -> write outputs -> queue -> send: ecrt_master_receive()/
    // ecrt_domain_process() refresh the whole process image (including
    // output bytes) from the just-received frame, so a write applied before
    // this call -- e.g. by the caller, before calling updateIO() at all --
    // gets silently overwritten right here and never reaches ecrt_domain_
    // queue()/ecrt_master_send(). applyWrites() has to run in between.
    void updateIO(const std::function<void()>& applyWrites) override {
        ecrt_master_receive(master_);
        ecrt_domain_process(domain_);
        applyWrites();
        ecrt_domain_queue(domain_);
        ecrt_master_send(master_);
    }

    void shutdown() override {
        if (master_) ecrt_master_deactivate(master_);
    }

    bool supportsHotplug() const override { return true; }

    // ecrt_master()/ecrt_master_get_slave() are documented safe to call in
    // either master phase (idle or operational), so this can run while the
    // cyclic loop is live -- but ecrt_master_get_slave() is "blocking", so
    // this is meant to be polled occasionally, not from inside the
    // real-time per-cycle section.
    bool topologyChanged() override {
        ec_master_info_t info{};
        if (ecrt_master(master_, &info) != 0) return false;
        if (info.slave_count != slaves_.size()) return true;

        for (unsigned int pos = 0; pos < info.slave_count; ++pos) {
            ec_slave_info_t si{};
            if (ecrt_master_get_slave(master_, static_cast<uint16_t>(pos), &si) != 0)
                return true; // can't even ask anymore -- treat as changed
            auto& ds = slaves_[pos];
            if (si.vendor_id != ds.vendorId || si.product_code != ds.productCode ||
                si.revision_number != ds.revisionNo)
                return true;
        }
        return false;
    }

    // See the base class doc comment for the deactivate/rescan/reactivate
    // cost this incurs -- unavoidable given IGH's documented API contract.
    void reconfigure(const Config& cfg, EsiRepository& esiRepo) override {
        spdlog::info("IGH: topology change detected, reconfiguring ({} slave(s) currently)...", slaves_.size());

        if (ecrt_master_deactivate(master_) != 0) throw std::runtime_error("ecrt_master_deactivate failed");

        slaves_.clear();
        pending_.clear();

        // No domain-free call exists in IGH's public API -- the old domain_
        // is leaked (owned/released by the master itself, eventually, on
        // ecrt_release_master()). Acceptable for occasional hotplug events.
        domain_ = ecrt_master_create_domain(master_);
        if (!domain_) throw std::runtime_error("ecrt_master_create_domain failed");

        rescan(cfg, esiRepo);
        activate();
    }

private:
    // Builds sync/pdo/entry arrays from pdoList and hands them straight to
    // ecrt_slave_config_pdos(), which both configures the SM's CoE
    // assignment for us and makes exactly these entries available for
    // registration -- then registers each one against domain_ and appends a
    // SlaveVariable + pending_ offset to ds for every entry that lands.
    // Shared by the two callers that need to configure a PDO list IGH's own
    // live introspection wouldn't otherwise give us: an explicit
    // --pdo-config override, and the ESI-default fallback below for slaves
    // whose sync manager has no live PDOs to introspect at all. `label` is
    // just for the warning/log messages so the two cases stay distinguishable.
    void applyPdoList(ec_slave_config_t* sc, uint8_t syncIdx, ec_direction_t syncDir, DataDirection dir,
                       unsigned int pos, const std::vector<PdoOverridePdo>& pdoList, DiscoveredSlave& ds,
                       const char* label) {
        std::vector<std::vector<ec_pdo_entry_info_t>> entryStorage(pdoList.size());
        std::vector<ec_pdo_info_t> pdoInfos(pdoList.size());
        for (size_t i = 0; i < pdoList.size(); ++i) {
            for (auto& e : pdoList[i].entries)
                entryStorage[i].push_back({e.index, e.subIndex, static_cast<uint8_t>(e.bitLen)});
            pdoInfos[i] = {pdoList[i].pdoIndex, static_cast<unsigned int>(entryStorage[i].size()),
                           entryStorage[i].empty() ? nullptr : entryStorage[i].data()};
        }
        ec_sync_info_t syncCfg{syncIdx, syncDir, static_cast<unsigned int>(pdoInfos.size()), pdoInfos.data(),
                               EC_WD_DEFAULT};

        if (ecrt_slave_config_pdos(sc, 1, &syncCfg) != 0) {
            spdlog::warn("IGH: failed to apply {} PDO assignment on slave {} sync {}", label, pos, syncIdx);
            return;
        }

        for (unsigned int pdoPos = 0; pdoPos < entryStorage.size(); ++pdoPos) {
            for (unsigned int entryPos = 0; entryPos < entryStorage[pdoPos].size(); ++entryPos) {
                auto& e = entryStorage[pdoPos][entryPos];
                if (e.bit_length == 0 || e.index == 0) continue;

                unsigned int bitPosition = 0;
                int byteOffset =
                    ecrt_slave_config_reg_pdo_entry_pos(sc, syncIdx, pdoPos, entryPos, domain_, &bitPosition);
                if (byteOffset < 0) {
                    spdlog::warn("IGH: failed to register {} PDO entry {:#06x}:{} on slave {}", label, e.index,
                                 e.subindex, pos);
                    continue;
                }

                SlaveVariable v;
                v.index = e.index;
                v.subIndex = e.subindex;
                v.bitLength = e.bit_length;
                v.bitOffset = static_cast<uint8_t>(bitPosition);
                v.direction = dir;
                v.dataType = GuessDataTypeFromBitLength(e.bit_length); // refined via ESI later
                ds.variables.push_back(v);
                pending_.push_back({slaves_.size(), ds.variables.size() - 1, byteOffset});
            }
        }
    }

    // Scans the bus and (re)builds slaves_/pending_ from scratch. Must run
    // while the master is in idle phase (before the first activate(), or
    // just after a deactivate()) -- IGH's own docs: slave configuration
    // can't be altered once ecrt_master_activate() has run.
    void rescan(const Config& cfg, EsiRepository& esiRepo) {
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
            // slaveInfo.alias is the persistent SII "Configured Station
            // Alias" (0 if never set); falls back to ring position.
            ds.reportedCsa = slaveInfo.alias != 0 ? slaveInfo.alias : ds.ringCsa;
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

            const PdoOverride* ov =
                cfg.pdoOverrides.empty() ? nullptr
                                          : FindOverride(cfg.pdoOverrides, slaveInfo.vendor_id, slaveInfo.product_code,
                                                          slaveInfo.revision_number);

            for (uint8_t syncIdx = 0; syncIdx < slaveInfo.sync_count; ++syncIdx) {
                ec_sync_info_t sync{};
                if (ecrt_master_get_sync_manager(master_, static_cast<uint16_t>(pos), syncIdx, &sync) != 0) continue;
                if (sync.dir != EC_DIR_OUTPUT && sync.dir != EC_DIR_INPUT) continue; // mailbox SMs etc.

                DataDirection dir = (sync.dir == EC_DIR_OUTPUT) ? DataDirection::Output : DataDirection::Input;

                const std::vector<PdoOverridePdo>* overridePdos =
                    ov ? (dir == DataDirection::Output ? &ov->rxPdos : &ov->txPdos) : nullptr;
                if (overridePdos && !overridePdos->empty()) {
                    // A custom assignment was requested for this direction.
                    // ecrt_master_get_pdo/_pdo_entry below reflect what's
                    // live *right now*, not our override (that only lands on
                    // the wire later, during the master's own state-
                    // transition sequence around activation) -- so build our
                    // own PDO/entry arrays from the override (already
                    // resolved against ESI by main.cpp) instead of trusting
                    // live introspection for this sync manager.
                    applyPdoList(sc, syncIdx, sync.dir, dir, pos, *overridePdos, ds, "overridden");
                    continue; // this sync manager is fully handled by the override
                }

                if (sync.n_pdos == 0) {
                    // Live introspection found nothing to enumerate for this
                    // sync manager -- e.g. a hardwired-mapping terminal with
                    // neither a CoE mailbox nor an SII PDO-assignment
                    // category (ecrt_master_get_sync_manager/_pdo/_pdo_entry
                    // read from whichever of those the slave actually has;
                    // some basic I/O terminals have neither). SOEM hits the
                    // analogous case by falling back to an ESI-derived
                    // mapping (see AddOpaqueFallback + main.cpp's
                    // ExpandOpaqueFromEsi); IGH has no equivalent "opaque
                    // buffer of N bytes" to fall back to (its introspection
                    // struct carries no SM byte-length), so instead resolve
                    // the device against ESI directly here and, if it
                    // declares a default mapping for this direction, apply
                    // that -- same source, applied a different way to fit
                    // IGH's API.
                    const EsiDevice* esiDevice =
                        esiRepo.resolve(slaveInfo.vendor_id, slaveInfo.product_code, slaveInfo.revision_number);
                    if (esiDevice) {
                        std::vector<PdoOverridePdo> esiDefault;
                        for (auto& p : esiDevice->pdos) {
                            if (p.direction != dir) continue;
                            PdoOverridePdo pdo;
                            pdo.pdoIndex = p.index;
                            for (auto& e : p.entries) pdo.entries.push_back({e.index, e.subIndex, e.bitLen});
                            esiDefault.push_back(std::move(pdo));
                        }
                        if (!esiDefault.empty())
                            applyPdoList(sc, syncIdx, sync.dir, dir, pos, esiDefault, ds, "ESI-default");
                    }
                    continue;
                }

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
