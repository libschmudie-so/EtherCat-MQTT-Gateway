// SOEM backend: enumerates live PDO mapping via CoE SDO reads of 0x1C12
// (RxPDO assign -> Output direction) / 0x1C13 (TxPDO assign -> Input
// direction), the standard ETG.1000.6 CoE PDO-assignment protocol (the same
// technique SOEM's own "slaveinfo" example tool uses). Slaves with no CoE
// mailbox fall back to one opaque variable per direction spanning the whole
// mapped buffer.

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

namespace ecmqtt {
namespace {

constexpr int kIoMapSize = 4096;

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

void AddOpaqueFallback(DiscoveredSlave& ds) {
    if (ec_slave[ds.ringCsa].Obytes > 0) {
        SlaveVariable v;
        v.name = "raw_output";
        v.bitLength = static_cast<uint16_t>(ec_slave[ds.ringCsa].Obytes * 8);
        v.direction = DataDirection::Output;
        v.dataPtr = ec_slave[ds.ringCsa].outputs;
        v.dataType = EthercatDataType::OctetString;
        ds.variables.push_back(v);
    }
    if (ec_slave[ds.ringCsa].Ibytes > 0) {
        SlaveVariable v;
        v.name = "raw_input";
        v.bitLength = static_cast<uint16_t>(ec_slave[ds.ringCsa].Ibytes * 8);
        v.direction = DataDirection::Input;
        v.dataPtr = ec_slave[ds.ringCsa].inputs;
        v.dataType = EthercatDataType::OctetString;
        ds.variables.push_back(v);
    }
}

class SoemBackend final : public IEtherCatBackend {
public:
    void configure(const Config& cfg) override {
        if (ec_init(cfg.interface.c_str()) <= 0)
            throw std::runtime_error("ec_init failed for interface '" + cfg.interface +
                                      "' (check the name and NET_RAW/NET_ADMIN permissions)");

        if (ec_config_init(FALSE) <= 0) {
            ec_close();
            throw std::runtime_error("No EtherCAT slaves found on " + cfg.interface);
        }

        ec_config_map(ioMap_.data());
        ec_configdc();

        ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

        slaves_.clear();
        for (int i = 1; i <= ec_slavecount; ++i) {
            DiscoveredSlave ds;
            ds.ringCsa = static_cast<uint16_t>(i);
            ds.reportedCsa = ec_slave[i].configadr;
            ds.vendorId = ec_slave[i].eep_man;
            ds.productCode = ec_slave[i].eep_id;
            ds.revisionNo = ec_slave[i].eep_rev;
            ds.liveName = ec_slave[i].name;

            bool outOk = EnumerateDirection(static_cast<uint16_t>(i), 0x1C12, DataDirection::Output,
                                             ec_slave[i].outputs, ds.variables);
            bool inOk = EnumerateDirection(static_cast<uint16_t>(i), 0x1C13, DataDirection::Input,
                                            ec_slave[i].inputs, ds.variables);

            if (!outOk && !inOk) {
                spdlog::debug("Slave {} has no CoE mailbox; exposing raw I/O buffers", i);
                AddOpaqueFallback(ds);
            }

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

        int chk = 40;
        do {
            ec_send_processdata();
            ec_receive_processdata(EC_TIMEOUTRET);
            ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
        } while (--chk > 0 && ec_slave[0].state != EC_STATE_OPERATIONAL);

        if (ec_slave[0].state != EC_STATE_OPERATIONAL) {
            ec_close();
            throw std::runtime_error("Not all EtherCAT slaves reached OPERATIONAL state");
        }

        spdlog::info("SOEM: {} slave(s) at OPERATIONAL", ec_slavecount);
    }

    std::vector<DiscoveredSlave>& slaves() override { return slaves_; }

    void updateIO() override {
        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);
    }

    void shutdown() override {
        ec_slave[0].state = EC_STATE_INIT;
        ec_writestate(0);
        ec_close();
    }

private:
    std::array<char, kIoMapSize> ioMap_{};
    std::vector<DiscoveredSlave> slaves_;
};

} // namespace

std::unique_ptr<IEtherCatBackend> createBackend() { return std::make_unique<SoemBackend>(); }

} // namespace ecmqtt
