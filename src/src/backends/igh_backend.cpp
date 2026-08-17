// IGH (Etherlab) master backend. Uses the live introspection API
// (ecrt_master_get_slave/_sync_manager/_pdo/_pdo_entry) to read each slave's
// currently-assigned PDO mapping (its SII/EEPROM default, same source SOEM's
// default config uses), then registers each entry by position
// (ecrt_slave_config_reg_pdo_entry_pos) to obtain a byte offset into a
// single shared domain. Struct layouts/field names checked directly against
// igh-ethercat-1.6.8/include/ecrt.h (ec_slave_info_t, ec_master_info_t,
// ec_sync_info_t, ec_pdo_info_t, ec_pdo_entry_info_t); re-check against your
// installed header if a different IGH version renames anything here.
//
// ec_pdo_info_t/ec_pdo_entry_info_t carry no name field -- IGH's live
// introspection, like SOEM's, has no human-readable PDO names. That's why
// ESI-file metadata is required for both backends, not just SOEM.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
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
        opWaitTimeoutMs_ = cfg.opWaitTimeoutMs;
        master_ = ecrt_request_master(0);
        if (!master_)
            throw std::runtime_error(
                "ecrt_request_master(0) failed. Is the ethercat master service running and device configured?");

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

        // ecrt_master_activate() only starts the cyclic phase -- unlike
        // SOEM's activate() (a blocking ec_statecheck loop), each slave's AL
        // state here only actually advances toward OP as the app keeps
        // driving receive/(process/queue)/send, the same exchange the real
        // cyclic loop runs afterward. Drive it here too and wait for every
        // slave to reach OP before returning, so a caller (main.cpp, after a
        // hotplug reconfigure in particular) doesn't treat the master as
        // ready before it actually is. Warn rather than throw on timeout:
        // one slow-to-come-up slave after a reconfigure shouldn't take the
        // whole gateway down (see the ecrt_domain_data null case above for
        // the same reasoning).
        //
        // Nothing to wait for with zero slaves -- and ecrt_domain_process()/
        // ecrt_domain_queue() are specifically skipped whenever domainBase
        // is null (a zero-size domain, same condition as above): unlike
        // ecrt_master_receive()/_send(), which operate on the whole master
        // regardless of domain content, these two are domain-specific calls
        // this code was never previously exercising with an empty domain --
        // updateIO() is the only other caller, and it only ever runs once
        // there's at least one slave. Untested territory otherwise.
        if (!slaves_.empty()) {
            const int maxChecks = std::max<int>(1, static_cast<int>(opWaitTimeoutMs_ / 50));
            bool allOp = false;
            for (int check = 0; check < maxChecks && !allOp; ++check) {
                ecrt_master_receive(master_);
                if (domainBase) {
                    ecrt_domain_process(domain_);
                    ecrt_domain_queue(domain_);
                }
                ecrt_master_send(master_);

                // RT-safe (ecrt_slave_config_state()), not
                // ecrt_master_get_slave() -- see RefreshSlaveStatesRt()'s
                // doc comment for why that distinction matters. Also
                // updates alStatusCode/alError as a side effect, and
                // doubles as the "final pass" the metadata needs: by the
                // time this loop exits, slaves_[].alState already reflects
                // its last iteration, so no separate pass is needed after.
                RefreshSlaveStatesRt();

                allOp = true;
                for (auto& ds : slaves_) {
                    if (ds.alState != SlaveAlState::Op) {
                        allOp = false;
                        break;
                    }
                }
                if (!allOp) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!allOp)
                spdlog::warn("IGH: not all slaves reached OPERATIONAL within the timeout after activation");
        }

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

        RefreshSlaveStatesRt();
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
    // Mirrors configure(): only rescans, doesn't activate() -- the caller
    // calls activate() separately afterward (same as it does after
    // configure()), which gives it a hook point between "bus rescanned" and
    // "slaves back at OP" to publish something in between if it wants to.
    void reconfigure(const Config& cfg, EsiRepository& esiRepo) override {
        spdlog::info("IGH: topology change detected, reconfiguring ({} slave(s) currently)...", slaves_.size());
        opWaitTimeoutMs_ = cfg.opWaitTimeoutMs;

        if (ecrt_master_deactivate(master_) != 0) throw std::runtime_error("ecrt_master_deactivate failed");

        slaves_.clear();
        pending_.clear();
        slaveConfigs_.clear();
        alStatusRequests_.clear();

        // No domain-free call exists in IGH's public API -- the old domain_
        // is leaked (owned/released by the master itself, eventually, on
        // ecrt_release_master()). Acceptable for occasional hotplug events.
        domain_ = ecrt_master_create_domain(master_);
        if (!domain_) throw std::runtime_error("ecrt_master_create_domain failed");

        rescan(cfg, esiRepo);
    }

private:
    // Refreshes alState/alStatusCode/alError for every slave from a live
    // read, called every updateIO() cycle. Both underlying calls are
    // documented rt_safe (apiusage{master_op,rt_safe} in ecrt.h) -- unlike
    // ecrt_master_get_slave(), which carries no such tag and, called
    // repeatedly this way instead, caused a real production incident (see
    // git history: it started failing outright under sustained polling and
    // cascaded into a reconfigure loop that kept dropping slaves). This is
    // a deliberately different, purpose-built mechanism, not a retry of
    // the same idea:
    //  - alState: ecrt_slave_config_state() against the ec_slave_config_t
    //    saved in slaveConfigs_ at rescan() time.
    //  - alStatusCode: a register request (alStatusRequests_, also created
    //    at rescan() time) reading ESC register 0x0134 (AL Status Code,
    //    ETG.1000.6). This is an async state machine per IGH's own design
    //    -- schedule a read, poll for completion, re-arm -- driven one step
    //    per call here rather than blocking for it.
    void RefreshSlaveStatesRt() {
        for (size_t i = 0; i < slaves_.size(); ++i) {
            if (i < slaveConfigs_.size() && slaveConfigs_[i]) {
                ec_slave_config_state_t state{};
                if (ecrt_slave_config_state(slaveConfigs_[i], &state) == 0)
                    slaves_[i].alState = SlaveAlStateFromRaw(static_cast<uint8_t>(state.al_state));
            }

            if (i >= alStatusRequests_.size() || !alStatusRequests_[i]) continue;
            ec_reg_request_t* req = alStatusRequests_[i];
            switch (ecrt_reg_request_state(req)) {
                case EC_REQUEST_UNUSED:
                    ecrt_reg_request_read(req, 0x0134, 2);
                    break;
                case EC_REQUEST_SUCCESS: {
                    uint16_t code = EC_READ_U16(ecrt_reg_request_data(req));
                    slaves_[i].alStatusCode = code;
                    slaves_[i].alError = code != 0;
                    ecrt_reg_request_read(req, 0x0134, 2); // re-arm for the next refresh
                    break;
                }
                case EC_REQUEST_ERROR:
                    ecrt_reg_request_read(req, 0x0134, 2); // re-arm and try again next cycle
                    break;
                case EC_REQUEST_BUSY:
                default:
                    break; // still in flight -- check again next cycle
            }
        }
    }

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
        for (size_t i = 0; i < pdoList.size(); ++i) {
            std::string entriesStr;
            for (auto& e : pdoList[i].entries)
                entriesStr += fmt::format("{}{:#06x}:{:#04x}/{}", entriesStr.empty() ? "" : ", ", e.index,
                                           e.subIndex, e.bitLen);
            spdlog::debug("IGH: slave {} sync {}: {} PDO[{}] = {:#06x} ({} entr{}: {})", pos, syncIdx, label, i,
                          pdoList[i].pdoIndex, pdoList[i].entries.size(),
                          pdoList[i].entries.size() == 1 ? "y" : "ies",
                          entriesStr.empty() ? "none" : entriesStr);
        }

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
    void rescan(const Config& cfg, EsiRepository& esiRepo, int attempt = 0) {
        ec_master_info_t info{};
        if (ecrt_master(master_, &info) != 0) throw std::runtime_error("ecrt_master() failed to obtain master info");

        // The master runs its own bus scan asynchronously (info.scan_busy)
        // -- right after ecrt_request_master(), or after a topology change,
        // slave_count can already reflect newly-seen slaves before their
        // SII EEPROM read (vendor/product/revision/name) has actually
        // finished. Querying a slave mid-scan doesn't fail
        // (ecrt_master_get_slave() still returns 0); it just hands back a
        // zeroed/incomplete ec_slave_info_t for that one. Waiting here for
        // scan_busy to clear is what makes slave_count and every per-slave
        // query below reflect a settled topology.
        auto scanWaitStart = std::chrono::steady_clock::now();
        while (info.scan_busy) {
            if (std::chrono::steady_clock::now() - scanWaitStart > std::chrono::seconds(30)) {
                spdlog::warn("IGH: master bus scan still in progress after 30s; proceeding anyway. Some slaves "
                             "may show up with an incomplete identity.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (ecrt_master(master_, &info) != 0)
                throw std::runtime_error("ecrt_master() failed to obtain master info");
        }

        slaves_.reserve(info.slave_count);
        bool anyZeroIdentity = false;

        for (unsigned int pos = 0; pos < info.slave_count; ++pos) {
            // Belt-and-braces on top of the scan_busy wait above: even with
            // that, a slave (typically one the topology change didn't even
            // touch) can occasionally still hand back a freshly-zeroed
            // ec_slave_info_t on the first ask -- vendor_id 0 is never a
            // real slave's identity, so retry a few times before accepting
            // it rather than latching an obviously-bogus reading.
            ec_slave_info_t slaveInfo{};
            bool gotSlaveInfo = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
                gotSlaveInfo = ecrt_master_get_slave(master_, static_cast<uint16_t>(pos), &slaveInfo) == 0;
                if (gotSlaveInfo && slaveInfo.vendor_id != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!gotSlaveInfo) {
                spdlog::warn("IGH: failed to get slave info at position {}", pos);
                continue;
            }
            if (slaveInfo.vendor_id == 0) {
                anyZeroIdentity = true;
                spdlog::warn(
                    "IGH: slave {} still reports vendor 0 after retrying for {}ms; using it anyway. Its name/ESI "
                    "match will likely be wrong until the next rescan.",
                    pos, 10 * 20);
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
            // As-of-scan-time state -- typically still INIT/PREOP/SAFEOP
            // here, well before activate() brings it to OP (which
            // overwrites this with the actually-reached state once it
            // runs). Populating it now rather than leaving it Unknown means
            // a metadata publish that lands before activate() -- e.g.
            // main.cpp's "rescanning"/"waiting" hotplug states -- still
            // shows something real instead of always Unknown.
            ds.alState = SlaveAlStateFromRaw(slaveInfo.al_state);
            ds.alError = slaveInfo.error_flag != 0;

            ec_slave_config_t* sc = ecrt_master_slave_config(master_, /*alias=*/0, static_cast<uint16_t>(pos),
                                                              slaveInfo.vendor_id, slaveInfo.product_code);
            if (!sc) {
                spdlog::warn("IGH: ecrt_master_slave_config failed for slave {} ('{}', vendor {:#x} product {:#x} "
                             "rev {:#x})",
                             pos, ds.liveName, ds.vendorId, ds.productCode, ds.revisionNo);
                slaveConfigs_.push_back(nullptr); // keeps index alignment with slaves_ for updateIO()
                alStatusRequests_.push_back(nullptr);
                slaves_.push_back(std::move(ds));
                continue;
            }

            slaveConfigs_.push_back(sc);
            // Reserved for the AL Status Code register (0x0134, 2 bytes) --
            // created here (non-realtime, before activate(), as
            // ecrt_slave_config_create_reg_request() requires) so updateIO()
            // can drive the actual read every cycle afterward via the
            // RT-safe ecrt_reg_request_read()/_state()/_data() calls. Null
            // if creation failed; updateIO() just skips a null entry and
            // alStatusCode stays at its default (0, "No error").
            alStatusRequests_.push_back(ecrt_slave_config_create_reg_request(sc, 2));

            const PdoOverride* ov =
                cfg.pdoOverrides.empty() ? nullptr
                                          : FindOverride(cfg.pdoOverrides, slaveInfo.vendor_id, slaveInfo.product_code,
                                                          slaveInfo.revision_number);

            for (uint8_t syncIdx = 0; syncIdx < slaveInfo.sync_count; ++syncIdx) {
                ec_sync_info_t sync{};
                if (ecrt_master_get_sync_manager(master_, static_cast<uint16_t>(pos), syncIdx, &sync) != 0) continue;
                // This does NOT actually exclude mailbox sync managers, despite
                // appearances: SM0/SM1 (conventionally Mailbox-Out/Mailbox-In
                // per ETG.1000.4, for any slave that has CoE at all) report the
                // exact same EC_DIR_OUTPUT/EC_DIR_INPUT values as real
                // process-data SMs -- IGH's public API has no distinct
                // "mailbox" direction. Kept only to drop whatever reports
                // neither (rare/invalid).
                if (sync.dir != EC_DIR_OUTPUT && sync.dir != EC_DIR_INPUT) continue;
                // The actual mailbox exclusion: skip SM0/SM1 for any slave
                // that has CoE (sdo_count > 0 is a reliable proxy -- a
                // mailbox-less slave's real process data can legitimately
                // start at SM0/1, so this must stay conditional). Confirmed
                // on real hardware (a Beckhoff EL3002): without this, a
                // --pdo-config override or the ESI-default fallback below
                // would get applied to SM1 (mailbox-in) right alongside the
                // real process-data SM, corrupting CoE mailbox communication
                // and leaving the slave stuck at PREOP+ERROR ("Invalid input
                // configuration").
                if (syncIdx < 2 && slaveInfo.sdo_count > 0) continue;

                DataDirection dir = (sync.dir == EC_DIR_OUTPUT) ? DataDirection::Output : DataDirection::Input;

                const std::vector<PdoOverridePdo>* overridePdos =
                    ov ? (dir == DataDirection::Output ? &ov->rxPdos : &ov->txPdos) : nullptr;
                if (overridePdos && !overridePdos->empty()) {
                    // main.cpp still records a selector that didn't resolve
                    // against ESI (unknown name, or a numeric index with no
                    // ESI match at all) as a PdoOverridePdo with empty
                    // entries -- fine for SOEM, which only ever writes the
                    // raw index to 0x1C12/0x1C13 and lets the slave supply
                    // its own entries. IGH has no such thing: passing
                    // n_entries=0 to ecrt_slave_config_pdos() means "use
                    // the slave's default mapping" per its own docs, which
                    // is NOT a no-op for a Fixed-PDO slave (e.g. an EL3012)
                    // -- observed on real hardware: the slave refuses the
                    // resulting remap ("does not support changing the PDO
                    // mapping", "Entries to map: (none)") and gets stuck in
                    // PREOP+ERROR ("Invalid input configuration") instead
                    // of keeping its already-working mapping. Drop any
                    // unresolved selector before it ever reaches the wire.
                    std::vector<PdoOverridePdo> resolved;
                    for (auto& p : *overridePdos)
                        if (!p.entries.empty()) resolved.push_back(p);
                    if (resolved.size() != overridePdos->size())
                        spdlog::warn(
                            "IGH: slave {} sync {}: {} of {} requested PDO override selector(s) didn't resolve "
                            "against ESI and will be skipped (IGH needs full entry content up front, unlike "
                            "SOEM). See the earlier '--pdo-config: ... not found' warning(s) for which.",
                            pos, syncIdx, overridePdos->size() - resolved.size(), overridePdos->size());

                    if (!resolved.empty()) {
                        // A custom assignment was requested for this
                        // direction. ecrt_master_get_pdo/_pdo_entry below
                        // reflect what's live *right now*, not our override
                        // (that only lands on the wire later, during the
                        // master's own state-transition sequence around
                        // activation) -- so build our own PDO/entry arrays
                        // from the override (already resolved against ESI
                        // by main.cpp) instead of trusting live
                        // introspection for this sync manager.
                        applyPdoList(sc, syncIdx, sync.dir, dir, pos, resolved, ds, "overridden");
                        continue; // this sync manager is fully handled by the override
                    }
                    // Nothing usable survived filtering -- fall through to
                    // the normal live-introspection path below, same as if
                    // no override had been requested for this direction at
                    // all, rather than leaving the sync manager unconfigured.
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
                            if (p.direction != dir || p.entries.empty()) continue;
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

        // A slave's identity (vendor/product/revision, read once from SII
        // during the bus scan) can get stuck all-zero independently of its
        // AL state (tracked live every cycle via a completely separate
        // path) -- observed on real hardware staying at OP the whole time
        // this happens. Retrying our own read harder (above) doesn't help
        // when it's the master's own cached record that's wrong, not our
        // read of it.
        //
        // Bounded to 4 attempts total, escalating each time -- an unbounded
        // retry would keep perturbing the bus for a slave with a genuine
        // fault that nothing here will fix:
        //  1, 2: `ethercat rescan` (the same real rescan the command-line
        //     tool commands -- shelling out to that actual tool rather than
        //     reimplementing its ioctl ourselves: that ioctl is defined in
        //     master/ioctl.h, which isn't part of the installed public API,
        //     only ecrt.h/ectty.h are, so unlike ecrt.h it carries no
        //     stability guarantee across IGH versions), then a settle delay
        //     before looking again -- rescan just commands the kernel to
        //     *start* a fresh scan asynchronously, so recursing immediately
        //     risks re-checking scan_busy before the kernel has even set
        //     it, seeing a stale "not busy" and reading the still-broken
        //     data right back. Empirically this alone can take more than
        //     one try.
        //  3: a harder reset than a topology rescan alone -- deactivate and
        //     recreate the domain here (the same "bus reinit" reconfigure()
        //     does for a hotplug event), then a longer settle delay.
        //  4 (final): harder still -- release and re-request the master
        //     entirely. Confirmed on real hardware that a slave surviving
        //     every attempt above can still clear on a full process
        //     restart (which does exactly this, via ~IghBackend() and a
        //     fresh ecrt_request_master() call), so replicate that
        //     in-process rather than requiring an actual restart. Every
        //     existing ec_slave_config_t*/ec_reg_request_t* (slaveConfigs_/
        //     alStatusRequests_) becomes invalid the instant master_ is
        //     released; clearing slaves_/pending_ below already implies
        //     starting the rest of this scan from scratch, so those two get
        //     cleared right alongside them and rebuilt fresh in the loop
        //     below regardless of which attempt tier actually ran.
        if (anyZeroIdentity && attempt < 4) {
            slaves_.clear();
            pending_.clear();
            slaveConfigs_.clear();
            alStatusRequests_.clear();

            if (attempt == 3) {
                spdlog::warn(
                    "IGH: still stuck after rescanning and a domain reinit; releasing and re-requesting "
                    "the master, the same recovery a full process restart provides (attempt {}/4)",
                    attempt + 1);
                ecrt_release_master(master_);
                master_ = ecrt_request_master(0);
                if (!master_)
                    throw std::runtime_error(
                        "ecrt_request_master(0) failed while recovering from a stuck-identity slave");
                domain_ = ecrt_master_create_domain(master_);
                if (!domain_)
                    throw std::runtime_error(
                        "ecrt_master_create_domain failed while recovering from a stuck-identity slave");
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } else {
                spdlog::warn(
                    "IGH: at least one slave has a stuck all-zero identity; commanding `ethercat rescan` "
                    "and retrying the scan (attempt {}/4)",
                    attempt + 1);
                if (std::system("ethercat rescan --master 0") != 0)
                    spdlog::warn("IGH: `ethercat rescan` failed or wasn't found -- is the IGH command-line "
                                 "tool installed and on PATH?");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                if (attempt == 2) {
                    spdlog::warn(
                        "IGH: still stuck after two rescans; reinitializing the bus (deactivate + "
                        "recreate domain) before continuing");
                    // Harmless if the master was never activated yet (e.g.
                    // this is the very first configure() of the process) --
                    // same reasoning reconfigure() already relies on by
                    // calling this unconditionally. No domain-free call
                    // exists in IGH's public API -- the old domain_ is
                    // leaked (owned/released by the master itself,
                    // eventually, on ecrt_release_master()), same as every
                    // other reinit here.
                    ecrt_master_deactivate(master_);
                    domain_ = ecrt_master_create_domain(master_);
                    if (!domain_)
                        throw std::runtime_error("ecrt_master_create_domain failed during stuck-identity recovery");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
            }

            rescan(cfg, esiRepo, attempt + 1);
        }
    }

    struct PendingOffset {
        size_t slaveArrIdx;
        size_t varIdx;
        int byteOffset;
    };

    ec_master_t* master_ = nullptr;
    ec_domain_t* domain_ = nullptr;
    uint32_t opWaitTimeoutMs_ = 2000;
    std::vector<DiscoveredSlave> slaves_;
    std::vector<PendingOffset> pending_;
    // Parallel to slaves_ (index-aligned, entries may be null if this
    // slave's ecrt_master_slave_config() failed). Both created in rescan(),
    // required to happen in the non-realtime idle phase before activate();
    // driven every cycle afterward from updateIO() via the RT-safe
    // ecrt_slave_config_state()/reg_request calls to keep alState/
    // alStatusCode live rather than frozen at whatever they were at the
    // last activate(). Neither is individually freed on a hotplug
    // reconfigure -- IGH's public API has no call for that, same as
    // domain_ above; both are owned/released by the master itself,
    // eventually, on ecrt_release_master().
    std::vector<ec_slave_config_t*> slaveConfigs_;
    std::vector<ec_reg_request_t*> alStatusRequests_;
};

} // namespace

std::unique_ptr<IEtherCatBackend> createBackend() { return std::make_unique<IghBackend>(); }

} // namespace ecmqtt
