// SOEM backend: enumerates live PDO mapping via CoE SDO reads of 0x1C12
// (RxPDO assign -> Output direction) / 0x1C13 (TxPDO assign -> Input
// direction), the standard ETG.1000.6 CoE PDO-assignment protocol (the same
// technique SOEM's own "slaveinfo" example tool uses). Slaves with no CoE
// mailbox fall back to one opaque variable per direction spanning the whole
// mapped buffer.

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

// spdlog/fmt must be parsed before ethercat.h: SOEM's osal_defs.h #defines a
// bare `PACKED` macro, which corrupts fmt's internal `template <bool PACKED,
// ...>` parameter names if it's already in scope when fmt's headers are parsed.
#include <spdlog/spdlog.h>

#include <ethercat.h>

#include "ecmqtt/ethercat_backend.hpp"
#include "ecmqtt/pdo_override.hpp"

namespace ecmqtt {
namespace {

constexpr int kIoMapSize = 4096;

// Set for the duration of configure() so Po2SoConfig() (a bare C function
// pointer -- SOEM's classic API leaves no room for a capture context) can
// find the requested overrides. SOEM's classic API is inherently
// single-master/global-state anyway (the whole ec_slave[] array is global),
// so this is consistent with the rest of it.
const std::vector<PdoOverride>* g_pdoOverrides = nullptr;

// Writes a 0x1C12 (RxPDO assign) / 0x1C13 (TxPDO assign) object: clear the
// count, write each requested PDO index, then set the new count -- the
// standard ETG.1000.6 sequence for changing which of a slave's declared
// PDOs are active.
bool WritePdoAssign(uint16_t slave, uint16_t assignIndex, const std::vector<PdoOverridePdo>& pdos) {
    uint16_t zero = 0;
    int l = sizeof(zero);
    if (ec_SDOwrite(slave, assignIndex, 0, FALSE, l, &zero, EC_TIMEOUTRXM) <= 0) return false;

    uint8_t n = 0;
    for (auto& pdo : pdos) {
        ++n;
        uint16_t idx = pdo.pdoIndex;
        l = sizeof(idx);
        if (ec_SDOwrite(slave, assignIndex, n, FALSE, l, &idx, EC_TIMEOUTRXM) <= 0) return false;
    }
    l = sizeof(n);
    return ec_SDOwrite(slave, assignIndex, 0, FALSE, l, &n, EC_TIMEOUTRXM) > 0;
}

// SOEM's standard hook point for CoE PDO reconfiguration: ec_config_map()
// calls this for every slave with it set, while the slave is in PRE-OP and
// before mapping is computed from the (now possibly just-changed) PDO
// assignment -- so a 0x1C12/0x1C13 write here is picked up automatically by
// the normal live enumeration in EnumerateDirection() below, no other
// change needed.
int Po2SoConfig(uint16 slave) {
    if (!g_pdoOverrides) return 0;
    for (auto& ov : *g_pdoOverrides) {
        if (ov.vendorId != ec_slave[slave].eep_man || ov.productCode != ec_slave[slave].eep_id ||
            ov.revisionNo != ec_slave[slave].eep_rev)
            continue;

        if (!ov.rxPdos.empty() && !WritePdoAssign(slave, 0x1C12, ov.rxPdos))
            spdlog::warn("Slave {}: failed to write custom RxPDO assignment (0x1C12)", slave);
        if (!ov.txPdos.empty() && !WritePdoAssign(slave, 0x1C13, ov.txPdos))
            spdlog::warn("Slave {}: failed to write custom TxPDO assignment (0x1C13)", slave);
        break;
    }
    return 0;
}

bool SdoReadU16(uint16_t slave, uint16_t index, uint8_t subindex, uint16_t& outVal) {
    uint8_t buf[2] = {0, 0};
    int l = sizeof(buf);
    int wc = ec_SDOread(slave, index, subindex, FALSE, &l, buf, EC_TIMEOUTRXM);
    if (wc <= 0) return false;
    outVal = static_cast<uint16_t>(buf[0] | (l > 1 ? (buf[1] << 8) : 0));
    return true;
}

bool SdoReadU32(uint16_t slave, uint16_t index, uint8_t subindex, uint32_t& outVal) {
    uint8_t buf[4] = {0, 0, 0, 0};
    int l = sizeof(buf);
    int wc = ec_SDOread(slave, index, subindex, FALSE, &l, buf, EC_TIMEOUTRXM);
    if (wc <= 0) return false;
    outVal = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
             (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
    return true;
}

// Returns false if the slave doesn't support CoE / this assign object at all
// (subindex 0 unreadable). A successful read with count==0 just means no
// PDOs are assigned in that direction, which is not a failure.
bool EnumerateDirection(uint16_t slaveIdx, uint16_t assignIndex, DataDirection dir, uint8_t* base,
                         std::vector<SlaveVariable>& out) {
    uint16_t count = 0;
    if (!SdoReadU16(slaveIdx, assignIndex, 0, count)) return false;

    uint32_t bitOffsetAccum = 0;
    for (uint16_t n = 1; n <= count; ++n) {
        uint16_t pdoIndex = 0;
        if (!SdoReadU16(slaveIdx, assignIndex, static_cast<uint8_t>(n), pdoIndex)) continue;

        uint16_t entryCount = 0;
        if (!SdoReadU16(slaveIdx, pdoIndex, 0, entryCount)) continue;

        for (uint16_t m = 1; m <= entryCount; ++m) {
            uint32_t packed = 0;
            if (!SdoReadU32(slaveIdx, pdoIndex, static_cast<uint8_t>(m), packed)) continue;

            uint16_t entryIndex = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
            uint8_t entrySub = static_cast<uint8_t>((packed >> 8) & 0xFF);
            uint8_t bitLen = static_cast<uint8_t>(packed & 0xFF);

            if (bitLen == 0) continue; // zero-length: malformed entry, skip without advancing

            if (entryIndex != 0) { // index 0 marks an alignment gap; still consumes bits below
                SlaveVariable v;
                v.index = entryIndex;
                v.subIndex = entrySub;
                v.bitLength = bitLen;
                v.direction = dir;
                v.dataPtr = base + (bitOffsetAccum / 8);
                v.bitOffset = static_cast<uint8_t>(bitOffsetAccum % 8);
                v.dataType = GuessDataTypeFromBitLength(bitLen); // refined later via ESI cross-reference
                out.push_back(v);
            }
            bitOffsetAccum += bitLen;
        }
    }
    return true;
}

// Adds a whole-buffer placeholder for one direction if that direction
// actually has mapped bytes. main.cpp's ExpandOpaqueFromEsi() replaces this
// with real per-entry fields (from ESI) when a match is found.
void CalcCrc(uint8_t& crc, uint8_t b) {
    crc ^= b;
    for (int j = 0; j <= 7; ++j) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
}

// SII EEPROM checksum: CRC-8/ATM (poly 0x07, init 0xFF) over the first 7
// words (14 bytes, the "General" category) -- ETG.2000. The slave
// controller validates this at reset and can refuse to boot from a
// corrupted EEPROM, so writing the alias word without recomputing this
// leaves the slave unable to come up. Matches SOEM's own eepromtool.c
// SIIcrc() exactly.
uint8_t SiiCrc(const uint8_t* buf) {
    uint8_t crc = 0xff;
    for (int i = 0; i <= 13; ++i) CalcCrc(crc, buf[i]);
    return crc;
}

// Reads `length` bytes starting at byte offset `start` from a slave's SII
// EEPROM (auto-increment addressed) into buf. Mirrors SOEM's own
// eepromtool.c eeprom_read(), including the 8-byte read-chunk path some
// slaves require (advertised via the EC_ESTAT_R64 status bit) -- getting
// the chunk width wrong misaligns the buffer.
void ReadEeprom(uint16_t aiadr, uint8_t* buf, int start, int length) {
    uint16_t estat = 0;
    ec_APRD(aiadr, ECT_REG_EEPSTAT, sizeof(estat), &estat, EC_TIMEOUTRET);
    estat = etohs(estat);

    int ainc = (estat & EC_ESTAT_R64) ? 8 : 4;
    for (int i = start; i < start + length; i += ainc) {
        uint64_t b = ec_readeepromAP(aiadr, static_cast<uint16_t>(i >> 1), EC_TIMEOUTEEP);
        for (int k = 0; k < ainc && (i + k) < start + length; ++k) buf[i + k] = static_cast<uint8_t>((b >> (8 * k)) & 0xFF);
    }
}

void AddOpaqueFallback(DiscoveredSlave& ds, DataDirection dir) {
    bool isOutput = dir == DataDirection::Output;
    int bytes = isOutput ? ec_slave[ds.ringCsa].Obytes : ec_slave[ds.ringCsa].Ibytes;
    if (bytes <= 0) return;

    SlaveVariable v;
    v.name = isOutput ? "raw_output" : "raw_input";
    v.bitLength = static_cast<uint16_t>(bytes * 8);
    v.direction = dir;
    v.dataPtr = isOutput ? ec_slave[ds.ringCsa].outputs : ec_slave[ds.ringCsa].inputs;
    v.dataType = EthercatDataType::OctetString;
    v.opaque = true;
    ds.variables.push_back(v);
}

class SoemBackend final : public IEtherCatBackend {
public:
    // esiRepo is unused here: SOEM's opaque-placeholder fallback for
    // CoE-less slaves (AddOpaqueFallback below) is expanded against ESI
    // entirely in main.cpp's ExpandOpaqueFromEsi, after this returns.
    void configure(const Config& cfg, EsiRepository& esiRepo) override {
        (void)esiRepo;
        opWaitTimeoutMs_ = cfg.opWaitTimeoutMs;
        if (ec_init(cfg.interface.c_str()) <= 0)
            throw std::runtime_error("ec_init failed for interface '" + cfg.interface +
                                      "' (check the name and NET_RAW/NET_ADMIN permissions)");

        if (ec_config_init(FALSE) <= 0) {
            ec_close();
            throw std::runtime_error("No EtherCAT slaves found on " + cfg.interface);
        }

        g_pdoOverrides = &cfg.pdoOverrides;
        if (!cfg.pdoOverrides.empty())
            for (int i = 1; i <= ec_slavecount; ++i) ec_slave[i].PO2SOconfig = &Po2SoConfig;

        ec_config_map(ioMap_.data());
        ec_configdc();

        ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

        slaves_.clear();
        for (int i = 1; i <= ec_slavecount; ++i) {
            DiscoveredSlave ds;
            ds.ringCsa = static_cast<uint16_t>(i);
            // ec_slave[i].configadr is just EC_NODEOFFSET + ring position --
            // assigned sequentially at scan time, not persistent. aliasadr
            // is the actual SII "Configured Station Alias" (0 if never set
            // via --write-alias), which is what survives the slave moving.
            ds.reportedCsa = ec_slave[i].aliasadr != 0 ? ec_slave[i].aliasadr : ds.ringCsa;
            ds.vendorId = ec_slave[i].eep_man;
            ds.productCode = ec_slave[i].eep_id;
            ds.revisionNo = ec_slave[i].eep_rev;
            ds.liveName = ec_slave[i].name;
            // As-of-scan-time state (typically SAFE-OP here, from the
            // ec_statecheck() a few lines up) -- activate() overwrites this
            // with the actually-reached state once it runs. Populating it
            // now means a metadata publish that lands before activate()
            // still shows something real instead of always Unknown.
            ds.alState = SlaveAlStateFromRaw(static_cast<uint8_t>(ec_slave[i].state));
            ds.alStatusCode = ec_slave[i].ALstatuscode;
            ds.alError = ec_slave[i].ALstatuscode != 0;

            // A direction's assign object (0x1C12/0x1C13) reading fine is not
            // enough: some slaves report a valid assign list but their
            // mapped PDOs (0x1600+/0x1A00+) aren't themselves readable as
            // SDOs (fixed/hardwired, only documented in ESI, not really in
            // the live object dictionary -- seen on Beckhoff EL2612), which
            // silently produces zero usable entries with no error at all.
            // Fall back per-direction on that outcome, not just on "no CoE
            // mailbox at all", so these terminals still get real fields once
            // ExpandOpaqueFromEsi (main.cpp) has an ESI match to expand from.
            size_t beforeOut = ds.variables.size();
            EnumerateDirection(static_cast<uint16_t>(i), 0x1C12, DataDirection::Output, ec_slave[i].outputs,
                                ds.variables);
            if (ds.variables.size() == beforeOut) AddOpaqueFallback(ds, DataDirection::Output);

            size_t beforeIn = ds.variables.size();
            EnumerateDirection(static_cast<uint16_t>(i), 0x1C13, DataDirection::Input, ec_slave[i].inputs,
                                ds.variables);
            if (ds.variables.size() == beforeIn) AddOpaqueFallback(ds, DataDirection::Input);

            slaves_.push_back(std::move(ds));
        }

        spdlog::info("SOEM: {} slave(s) at SAFE-OP, ready to activate", ec_slavecount);
    }

    // Pushes all slaves from SAFE-OP to OPERATIONAL. Slaves expect the
    // master to keep cycling them reliably once this starts (a sync-manager
    // watchdog trip is the typical symptom of not doing so promptly), so
    // per the base class contract this should run right before the cyclic
    // loop starts -- not with slow unrelated setup (MQTT connect, ESI
    // parsing) still to come afterward.
    void activate() override {
        ec_slave[0].state = EC_STATE_OPERATIONAL;
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);
        ec_writestate(0);

        // Each ec_statecheck() call below times out after 50ms on its own,
        // so --op-timeout / 50ms is how many of those we get to try.
        int chk = std::max<int>(1, static_cast<int>(opWaitTimeoutMs_ / 50));
        do {
            ec_send_processdata();
            ec_receive_processdata(EC_TIMEOUTRET);
            ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
        } while (--chk > 0 && ec_slave[0].state != EC_STATE_OPERATIONAL);

        if (ec_slave[0].state != EC_STATE_OPERATIONAL) {
            ec_close();
            throw std::runtime_error("Not all EtherCAT slaves reached OPERATIONAL state");
        }

        // ec_statecheck() above only tracks the aggregate state at index 0.
        RefreshSlaveStates();

        spdlog::info("SOEM: {} slave(s) at OPERATIONAL", ec_slavecount);
    }

    std::vector<DiscoveredSlave>& slaves() override { return slaves_; }

    // SOEM has no receive/process step that resets output memory, and
    // send() transmits immediately -- so applying writes right before
    // send() (same as this class always did) is already correct.
    void updateIO(const std::function<void()>& applyWrites) override {
        applyWrites();
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);

        // ec_readstate() is a single broadcast read covering every slave in
        // one datagram (not a per-slave call), so unlike the one IGH-side
        // function that turned out to be unsafe to call this often (see
        // igh_backend.cpp's RefreshSlaveStatesRt() doc comment), this is
        // the standard, lightweight way SOEM applications keep per-slave
        // state current every cycle.
        RefreshSlaveStates();
    }

    void shutdown() override {
        ec_slave[0].state = EC_STATE_INIT;
        ec_writestate(0);
        ec_close();
    }

    // Standalone tool operation -- see the base class doc comment. Mirrors
    // SOEM's own bundled eepromtool/aliastool reference implementation: a
    // bare slave-count scan (no ec_config_init/ec_config_map), then for each
    // requested slave, force EEPROM control to the master and write word
    // address 0x04 (the SII "Configured Station Alias") via auto-increment
    // addressing.
    bool writeAliases(const Config& cfg, const std::vector<std::pair<uint16_t, uint16_t>>& ringPosToAlias) override {
        if (ec_init(cfg.interface.c_str()) <= 0) {
            spdlog::critical("ec_init failed for interface '{}'", cfg.interface);
            return false;
        }

        uint16_t typeReg = 0;
        int wkc = ec_BRD(0x0000, ECT_REG_TYPE, sizeof(typeReg), &typeReg, EC_TIMEOUTSAFE);
        if (wkc <= 0) {
            spdlog::critical("No EtherCAT slaves found on {}", cfg.interface);
            ec_close();
            return false;
        }
        int slaveCount = wkc;
        spdlog::info("{} slave(s) found on {}", slaveCount, cfg.interface);

        bool allOk = true;
        for (auto& [ringPos, alias] : ringPosToAlias) {
            if (ringPos < 1 || ringPos > slaveCount) {
                spdlog::error("Ring position {} out of range (1..{})", ringPos, slaveCount);
                allOk = false;
                continue;
            }

            uint16_t aiadr = static_cast<uint16_t>(1 - static_cast<int>(ringPos));
            uint8_t eepctl = 2;
            ec_APWR(aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET); // force EEPROM from PDI
            eepctl = 0;
            ec_APWR(aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET); // EEPROM control to master

            // Read the current first 14 bytes, patch in the new alias word,
            // and recompute the checksum over the patched buffer -- writing
            // just the alias word and leaving the old checksum in place is
            // what corrupts the EEPROM (the ESC refuses to load it at the
            // next reset, i.e. the slave fails to boot).
            uint8_t buf[14] = {};
            ReadEeprom(aiadr, buf, 0, sizeof(buf));
            buf[0x08] = static_cast<uint8_t>(alias & 0xFF);
            buf[0x09] = static_cast<uint8_t>((alias >> 8) & 0xFF);
            uint8_t crc = SiiCrc(buf);

            if (ec_writeeepromAP(aiadr, 0x04, alias, EC_TIMEOUTEEP) <= 0 ||
                ec_writeeepromAP(aiadr, 0x07, crc, EC_TIMEOUTEEP) <= 0) {
                spdlog::error("Failed to write alias {:#06x} (or its checksum) to slave at ring position {}", alias,
                              ringPos);
                allOk = false;
                continue;
            }
            spdlog::info(
                "Wrote alias {:#06x} to slave at ring position {}. Power-cycle that slave for the new alias "
                "to take effect; it's latched by the EtherCAT slave controller at reset, not live.",
                alias, ringPos);
        }

        ec_close();
        return allOk;
    }

private:
    // Refreshes alState/alStatusCode/alError for every slave from a single
    // broadcast read (see updateIO()'s doc comment on why this is safe to
    // call every cycle, unlike the analogous IGH mechanism's original,
    // unsafe attempt).
    void RefreshSlaveStates() {
        ec_readstate();
        for (int i = 1; i <= ec_slavecount; ++i) {
            auto& ds = slaves_[i - 1];
            ds.alState = SlaveAlStateFromRaw(ec_slave[i].state);
            ds.alStatusCode = ec_slave[i].ALstatuscode;
            ds.alError = ec_slave[i].ALstatuscode != 0;
        }
    }

    std::array<char, kIoMapSize> ioMap_{};
    uint32_t opWaitTimeoutMs_ = 2000;
    std::vector<DiscoveredSlave> slaves_;
};

} // namespace

std::unique_ptr<IEtherCatBackend> createBackend() { return std::make_unique<SoemBackend>(); }

} // namespace ecmqtt
