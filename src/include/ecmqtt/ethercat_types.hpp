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

// A slave's EtherCAT application-layer (AL) state, ETG.1000.6 encoding --
// the same values both backends' live introspection reports (IGH:
// ec_slave_info_t::al_state; SOEM: ec_slave[]::state, low nibble).
enum class SlaveAlState : uint8_t {
    Unknown = 0,
    Init = 1,
    PreOp = 2,
    Boot = 3,
    SafeOp = 4,
    Op = 8,
};

// Maps a raw AL status byte to the enum, masking off bit 4 (the "error
// indication acknowledge" flag ETG.1000 defines alongside the state in the
// same byte) first. Unrecognized values map to Unknown.
SlaveAlState SlaveAlStateFromRaw(uint8_t raw);

const char* ToString(SlaveAlState state);

// Human-readable text for a slave's AL Status Code (ESC register 0x0134,
// ETG.1000.6 Annex) -- e.g. 0x001E -> "Invalid input configuration". 0
// ("No error") is a valid, normal code, not a placeholder for "unknown".
// Table mirrors IGH's own (master/fsm_change.c al_status_messages[]) so the
// text matches what shows up in `dmesg` for the same code; falls back to a
// generic "Unknown status code" for anything not in the standard table.
std::string AlStatusMessage(uint16_t code);

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
