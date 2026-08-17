#include "ecmqtt/ethercat_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ecmqtt {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}
} // namespace

EthercatDataType EsiDataTypeFromString(const std::string& esiTypeIn) {
    // ESI DataType strings sometimes carry a length suffix, e.g. "STRING(20)".
    auto paren = esiTypeIn.find('(');
    std::string esiType = upper(paren == std::string::npos ? esiTypeIn : esiTypeIn.substr(0, paren));

    if (esiType == "BOOL" || esiType == "BOOLEAN") return EthercatDataType::Boolean;
    if (esiType == "BIT1") return EthercatDataType::Boolean;
    if (esiType == "BIT2") return EthercatDataType::Bit2;
    if (esiType == "BIT3") return EthercatDataType::Bit3;
    if (esiType == "BIT4") return EthercatDataType::Bit4;
    if (esiType == "BIT5") return EthercatDataType::Bit5;
    if (esiType == "BIT6") return EthercatDataType::Bit6;
    if (esiType == "BIT7") return EthercatDataType::Bit7;
    if (esiType == "BIT8") return EthercatDataType::Bit8;
    if (esiType == "BITARR8") return EthercatDataType::BitArr8;
    if (esiType == "BITARR16") return EthercatDataType::BitArr16;
    if (esiType == "BITARR32") return EthercatDataType::BitArr32;

    if (esiType == "USINT" || esiType == "UINT8" || esiType == "BYTE") return EthercatDataType::Unsigned8;
    if (esiType == "UINT" || esiType == "UINT16" || esiType == "WORD") return EthercatDataType::Unsigned16;
    if (esiType == "UINT24") return EthercatDataType::Unsigned24;
    if (esiType == "UDINT" || esiType == "UINT32" || esiType == "DWORD") return EthercatDataType::Unsigned32;
    if (esiType == "UINT40") return EthercatDataType::Unsigned40;
    if (esiType == "UINT48") return EthercatDataType::Unsigned48;
    if (esiType == "UINT56") return EthercatDataType::Unsigned56;
    if (esiType == "ULINT" || esiType == "UINT64") return EthercatDataType::Unsigned64;

    if (esiType == "SINT" || esiType == "INT8") return EthercatDataType::Integer8;
    if (esiType == "INT" || esiType == "INT16") return EthercatDataType::Integer16;
    if (esiType == "INT24") return EthercatDataType::Integer24;
    if (esiType == "DINT" || esiType == "INT32") return EthercatDataType::Integer32;
    if (esiType == "INT40") return EthercatDataType::Integer40;
    if (esiType == "INT48") return EthercatDataType::Integer48;
    if (esiType == "INT56") return EthercatDataType::Integer56;
    if (esiType == "LINT" || esiType == "INT64") return EthercatDataType::Integer64;

    if (esiType == "REAL" || esiType == "REAL32") return EthercatDataType::Float;
    if (esiType == "LREAL" || esiType == "REAL64") return EthercatDataType::Double;

    if (esiType == "STRING" || esiType == "VISIBLESTRING") return EthercatDataType::VisibleString;
    if (esiType == "OCTETSTRING") return EthercatDataType::OctetString;

    return EthercatDataType::Unknown;
}

EthercatDataType GuessDataTypeFromBitLength(uint16_t bitLength) {
    switch (bitLength) {
        case 1: return EthercatDataType::Boolean;
        case 8: return EthercatDataType::Unsigned8;
        case 16: return EthercatDataType::Unsigned16;
        case 32: return EthercatDataType::Unsigned32;
        case 64: return EthercatDataType::Unsigned64;
        default: return EthercatDataType::BitArr32;
    }
}

const char* ToString(EthercatDataType type) {
    switch (type) {
        case EthercatDataType::Boolean: return "Boolean";
        case EthercatDataType::Bit2: return "BIT2";
        case EthercatDataType::Bit3: return "BIT3";
        case EthercatDataType::Bit4: return "BIT4";
        case EthercatDataType::Bit5: return "BIT5";
        case EthercatDataType::Bit6: return "BIT6";
        case EthercatDataType::Bit7: return "BIT7";
        case EthercatDataType::Bit8: return "BIT8";
        case EthercatDataType::BitArr8: return "BITARR8";
        case EthercatDataType::BitArr16: return "BITARR16";
        case EthercatDataType::BitArr32: return "BITARR32";
        case EthercatDataType::Unsigned8: return "Unsigned8";
        case EthercatDataType::Unsigned16: return "Unsigned16";
        case EthercatDataType::Unsigned24: return "Unsigned24";
        case EthercatDataType::Unsigned32: return "Unsigned32";
        case EthercatDataType::Unsigned40: return "Unsigned40";
        case EthercatDataType::Unsigned48: return "Unsigned48";
        case EthercatDataType::Unsigned56: return "Unsigned56";
        case EthercatDataType::Unsigned64: return "Unsigned64";
        case EthercatDataType::Integer8: return "Integer8";
        case EthercatDataType::Integer16: return "Integer16";
        case EthercatDataType::Integer24: return "Integer24";
        case EthercatDataType::Integer32: return "Integer32";
        case EthercatDataType::Integer40: return "Integer40";
        case EthercatDataType::Integer48: return "Integer48";
        case EthercatDataType::Integer56: return "Integer56";
        case EthercatDataType::Integer64: return "Integer64";
        case EthercatDataType::Float: return "Float";
        case EthercatDataType::Double: return "Double";
        case EthercatDataType::VisibleString: return "VisibleString";
        case EthercatDataType::OctetString: return "OctetString";
        default: return "Unknown";
    }
}

const char* ToString(DataDirection dir) {
    return dir == DataDirection::Input ? "Input" : "Output";
}

SlaveAlState SlaveAlStateFromRaw(uint8_t raw) {
    switch (raw & 0x0F) {
        case 0x01: return SlaveAlState::Init;
        case 0x02: return SlaveAlState::PreOp;
        case 0x03: return SlaveAlState::Boot;
        case 0x04: return SlaveAlState::SafeOp;
        case 0x08: return SlaveAlState::Op;
        default: return SlaveAlState::Unknown;
    }
}

const char* ToString(SlaveAlState state) {
    switch (state) {
        case SlaveAlState::Init: return "INIT";
        case SlaveAlState::PreOp: return "PREOP";
        case SlaveAlState::Boot: return "BOOT";
        case SlaveAlState::SafeOp: return "SAFEOP";
        case SlaveAlState::Op: return "OP";
        default: return "UNKNOWN";
    }
}

namespace {
struct AlStatusEntry {
    uint16_t code;
    const char* message;
};

// ETG.1000.6 standard AL Status Codes, copied verbatim from IGH's own table
// (master/fsm_change.c al_status_messages[]) so the text matches dmesg
// exactly for the same code.
constexpr AlStatusEntry kAlStatusMessages[] = {
    {0x0000, "No error"},
    {0x0001, "Unspecified error"},
    {0x0002, "No Memory"},
    {0x0011, "Invalid requested state change"},
    {0x0012, "Unknown requested state"},
    {0x0013, "Bootstrap not supported"},
    {0x0014, "No valid firmware"},
    {0x0015, "Invalid mailbox configuration"},
    {0x0016, "Invalid mailbox configuration"},
    {0x0017, "Invalid sync manager configuration"},
    {0x0018, "No valid inputs available"},
    {0x0019, "No valid outputs"},
    {0x001A, "Synchronization error"},
    {0x001B, "Sync manager watchdog"},
    {0x001C, "Invalid sync manager types"},
    {0x001D, "Invalid output configuration"},
    {0x001E, "Invalid input configuration"},
    {0x001F, "Invalid watchdog configuration"},
    {0x0020, "Slave needs cold start"},
    {0x0021, "Slave needs INIT"},
    {0x0022, "Slave needs PREOP"},
    {0x0023, "Slave needs SAFEOP"},
    {0x0024, "Invalid Input Mapping"},
    {0x0025, "Invalid Output Mapping"},
    {0x0026, "Inconsistent Settings"},
    {0x0027, "Freerun not supported"},
    {0x0028, "Synchronization not supported"},
    {0x0029, "Freerun needs 3 Buffer Mode"},
    {0x002A, "Background Watchdog"},
    {0x002B, "No Valid Inputs and Outputs"},
    {0x002C, "Fatal Sync Error"},
    {0x002D, "No Sync Error"},
    {0x0030, "Invalid DC SYNCH configuration"},
    {0x0031, "Invalid DC latch configuration"},
    {0x0032, "PLL error"},
    {0x0033, "DC Sync IO Error"},
    {0x0034, "DC Sync Timeout Error"},
    {0x0035, "DC Invalid Sync Cycle Time"},
    {0x0036, "DC Sync0 Cycle Time"},
    {0x0037, "DC Sync1 Cycle Time"},
    {0x0041, "MBX_AOE"},
    {0x0042, "MBX_EOE"},
    {0x0043, "MBX_COE"},
    {0x0044, "MBX_FOE"},
    {0x0045, "MBX_SOE"},
    {0x004F, "MBX_VOE"},
    {0x0050, "EEPROM No Access"},
    {0x0051, "EEPROM Error"},
    {0x0060, "Slave Restarted Locally"},
};
} // namespace

std::string AlStatusMessage(uint16_t code) {
    for (auto& entry : kAlStatusMessages)
        if (entry.code == code) return entry.message;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Unknown status code 0x%04X", code);
    return buf;
}

} // namespace ecmqtt
