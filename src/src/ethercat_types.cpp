#include "ecmqtt/ethercat_types.hpp"

#include <algorithm>
#include <cctype>

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

} // namespace ecmqtt
