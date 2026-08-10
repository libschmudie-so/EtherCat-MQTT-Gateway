#pragma once

#include <cstdint>
#include <string>

namespace ecmqtt {

// Mirrors the CANopen/EtherCAT primitive data types used for PDO entries.
enum class EthercatDataType {
    Unknown = 0,
    Boolean,
    Bit2, Bit3, Bit4, Bit5, Bit6, Bit7, Bit8,
    BitArr8, BitArr16, BitArr32,
    Unsigned8, Unsigned16, Unsigned24, Unsigned32, Unsigned40, Unsigned48, Unsigned56, Unsigned64,
    Integer8, Integer16, Integer24, Integer32, Integer40, Integer48, Integer56, Integer64,
    Float, Double,
    VisibleString, OctetString,
};

enum class DataDirection {
    Input,  // slave -> master (TxPDO), published to MQTT
    Output, // master -> slave (RxPDO), writable via MQTT
};

// Best-effort mapping from an ESI <DataType> string (e.g. "BOOL", "UINT16",
// "REAL32") to our enum. Falls back to Unknown for anything unrecognized.
EthercatDataType EsiDataTypeFromString(const std::string& esiType);

// Heuristic fallback when no ESI match exists for an entry: infer a type
// purely from bit length, matching the original tool's best-effort behavior
// when metadata can't be resolved.
EthercatDataType GuessDataTypeFromBitLength(uint16_t bitLength);

const char* ToString(EthercatDataType type);
const char* ToString(DataDirection dir);

} // namespace ecmqtt
