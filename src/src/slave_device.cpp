#include "ecmqtt/slave_device.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ecmqtt {

namespace {

// Extracts bitLength bits starting at bitOffset (0-7) within base into a
// tightly packed, LSB-first buffer -- mirrors SlaveDevice.cs's ReadVariable.
std::vector<uint8_t> ReadBits(const uint8_t* base, uint8_t bitOffset, uint16_t bitLength) {
    std::vector<uint8_t> buf((bitLength + 7) / 8, 0);
    for (uint16_t i = 0; i < bitLength; ++i) {
        int srcByte = (bitOffset + i) / 8;
        int srcBit = (bitOffset + i) % 8;
        if (base[srcByte] & (1 << srcBit))
            buf[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    }
    return buf;
}

void WriteBits(uint8_t* base, uint8_t bitOffset, uint16_t bitLength, const uint8_t* value) {
    for (uint16_t i = 0; i < bitLength; ++i) {
        int dstByte = (bitOffset + i) / 8;
        int dstBit = (bitOffset + i) % 8;
        if (value[i / 8] & (1 << (i % 8)))
            base[dstByte] |= static_cast<uint8_t>(1 << dstBit);
        else
            base[dstByte] &= static_cast<uint8_t>(~(1 << dstBit));
    }
}

uint64_t BufferToUInt(const uint8_t* buf, uint16_t bits) {
    uint64_t result = 0;
    for (uint16_t i = 0; i < bits; ++i) {
        int byteIndex = i / 8;
        int bitIndex = i % 8;
        if (buf[byteIndex] & (1 << bitIndex))
            result |= (1ULL << i);
    }
    return result;
}

int64_t BufferToInt(const uint8_t* buf, uint16_t bits) {
    uint64_t u = BufferToUInt(buf, bits);
    int64_t s = static_cast<int64_t>(u);
    if (bits < 64 && ((1LL << (bits - 1)) & s) != 0)
        s |= (-1LL << bits); // sign-extend
    return s;
}

std::vector<uint8_t> UIntToBuffer(uint64_t value, uint16_t bits) {
    std::vector<uint8_t> buf((bits + 7) / 8, 0);
    for (uint16_t i = 0; i < bits; ++i) {
        if (value & (1ULL << i))
            buf[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    }
    return buf;
}

std::vector<uint8_t> IntToBuffer(int64_t value, uint16_t bits) {
    return UIntToBuffer(static_cast<uint64_t>(value), bits);
}

bool IsHexDigits(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string ToHexString(const std::vector<uint8_t>& buf) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(buf.size() * 2);
    for (uint8_t b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

std::vector<uint8_t> FromHexString(const std::string& s) {
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(std::stoul(s.substr(i * 2, 2), nullptr, 16));
    return out;
}

} // namespace

SlaveDevice::SlaveDevice(DiscoveredSlave& slave, std::string name, std::string description)
    : slave_(&slave), name_(std::move(name)), description_(std::move(description)) {}

nlohmann::json SlaveDevice::GetMetadata() const {
    nlohmann::json pdos = nlohmann::json::array();
    for (const auto& v : slave_->variables) {
        char indexHex[8];
        char subHex[8];
        std::snprintf(indexHex, sizeof(indexHex), "%04X", v.index);
        std::snprintf(subHex, sizeof(subHex), "%02X", v.subIndex);

        pdos.push_back({
            {"name", v.name},
            {"description", v.description},
            {"bitLength", v.bitLength},
            {"dataType", ToString(v.dataType)},
            {"dataDirection", ToString(v.direction)},
            {"index", indexHex},
            {"subIndex", subHex},
        });
    }

    return {
        {"name", name_},
        {"description", description_},
        {"state", ToString(slave_->alState)},
        {"reportedCsa", slave_->reportedCsa},
        {"ringCsa", slave_->ringCsa},
        {"pdos", pdos},
    };
}

std::vector<SlaveVariable*> SlaveDevice::GetAllVariables() {
    std::vector<SlaveVariable*> result;
    for (auto& v : slave_->variables)
        if (v.dataPtr != nullptr)
            result.push_back(&v);
    return result;
}

std::vector<SlaveVariable*> SlaveDevice::GetInputVariables() {
    std::vector<SlaveVariable*> result;
    for (auto& v : slave_->variables)
        if (v.dataPtr != nullptr && v.direction == DataDirection::Input)
            result.push_back(&v);
    return result;
}

std::vector<SlaveVariable*> SlaveDevice::GetOutputVariables() {
    std::vector<SlaveVariable*> result;
    for (auto& v : slave_->variables)
        if (v.dataPtr != nullptr && v.direction == DataDirection::Output)
            result.push_back(&v);
    return result;
}

nlohmann::json SlaveDevice::ReadVariableAsJson(const SlaveVariable& variable) const {
    if (variable.dataPtr == nullptr) return nullptr;

    auto buf = ReadBits(variable.dataPtr, variable.bitOffset, variable.bitLength);
    const uint8_t* p = buf.data();

    switch (variable.dataType) {
        case EthercatDataType::Boolean:
            return (BufferToUInt(p, variable.bitLength) & 1ULL) == 1ULL;

        case EthercatDataType::Bit2: case EthercatDataType::Bit3: case EthercatDataType::Bit4:
        case EthercatDataType::Bit5: case EthercatDataType::Bit6: case EthercatDataType::Bit7:
        case EthercatDataType::Bit8:
        case EthercatDataType::BitArr8: case EthercatDataType::BitArr16: case EthercatDataType::BitArr32:
            return BufferToUInt(p, variable.bitLength);

        case EthercatDataType::Unsigned8:
            return static_cast<uint8_t>(BufferToUInt(p, 8));
        case EthercatDataType::Unsigned16: {
            uint16_t v; std::memcpy(&v, p, 2); return v;
        }
        case EthercatDataType::Unsigned24:
            return static_cast<uint32_t>(BufferToUInt(p, 24));
        case EthercatDataType::Unsigned32: {
            uint32_t v; std::memcpy(&v, p, 4); return v;
        }
        case EthercatDataType::Unsigned40: case EthercatDataType::Unsigned48:
        case EthercatDataType::Unsigned56: case EthercatDataType::Unsigned64:
            return BufferToUInt(p, variable.bitLength);

        case EthercatDataType::Integer8:
            return static_cast<int8_t>(BufferToInt(p, 8));
        case EthercatDataType::Integer16: {
            int16_t v; std::memcpy(&v, p, 2); return v;
        }
        case EthercatDataType::Integer24:
            return static_cast<int32_t>(BufferToInt(p, 24));
        case EthercatDataType::Integer32: {
            int32_t v; std::memcpy(&v, p, 4); return v;
        }
        case EthercatDataType::Integer40: case EthercatDataType::Integer48:
        case EthercatDataType::Integer56: case EthercatDataType::Integer64:
            return BufferToInt(p, variable.bitLength);

        case EthercatDataType::Float: {
            float v; std::memcpy(&v, p, 4); return v;
        }
        case EthercatDataType::Double: {
            double v; std::memcpy(&v, p, 8); return v;
        }

        case EthercatDataType::VisibleString: {
            auto nul = std::find(buf.begin(), buf.end(), 0);
            return std::string(buf.begin(), nul);
        }

        case EthercatDataType::OctetString:
            return ToHexString(buf);

        default:
            return nullptr;
    }
}

void SlaveDevice::WriteVariableAsJson(const SlaveVariable& variable, const nlohmann::json& value) {
    if (variable.dataPtr == nullptr) return;

    switch (variable.dataType) {
        case EthercatDataType::Boolean:
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength,
                      UIntToBuffer(value.get<bool>() ? 1ULL : 0ULL, variable.bitLength).data());
            return;

        case EthercatDataType::Bit2: case EthercatDataType::Bit3: case EthercatDataType::Bit4:
        case EthercatDataType::Bit5: case EthercatDataType::Bit6: case EthercatDataType::Bit7:
        case EthercatDataType::Bit8:
        case EthercatDataType::BitArr8: case EthercatDataType::BitArr16: case EthercatDataType::BitArr32:
        case EthercatDataType::Unsigned8: case EthercatDataType::Unsigned16: case EthercatDataType::Unsigned24:
        case EthercatDataType::Unsigned32: case EthercatDataType::Unsigned40: case EthercatDataType::Unsigned48:
        case EthercatDataType::Unsigned56: case EthercatDataType::Unsigned64:
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength,
                      UIntToBuffer(value.get<uint64_t>(), variable.bitLength).data());
            return;

        case EthercatDataType::Integer8: case EthercatDataType::Integer16: case EthercatDataType::Integer24:
        case EthercatDataType::Integer32: case EthercatDataType::Integer40: case EthercatDataType::Integer48:
        case EthercatDataType::Integer56: case EthercatDataType::Integer64:
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength,
                      IntToBuffer(value.get<int64_t>(), variable.bitLength).data());
            return;

        case EthercatDataType::Float: {
            float f = value.get<float>();
            std::array<uint8_t, 4> tmp;
            std::memcpy(tmp.data(), &f, 4);
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength, tmp.data());
            return;
        }
        case EthercatDataType::Double: {
            double d = value.get<double>();
            std::array<uint8_t, 8> tmp;
            std::memcpy(tmp.data(), &d, 8);
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength, tmp.data());
            return;
        }

        case EthercatDataType::VisibleString: {
            std::string s = value.get<std::string>();
            std::vector<uint8_t> bytes(s.begin(), s.end());
            bytes.resize((variable.bitLength + 7) / 8, 0);
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength, bytes.data());
            return;
        }

        case EthercatDataType::OctetString: {
            // Accepts a hex string, else raw UTF-8 bytes (the .NET original also
            // tried base64; omitted here since nothing else in this project needs it).
            std::string s = value.get<std::string>();
            std::vector<uint8_t> bytes;
            if (s.size() % 2 == 0 && IsHexDigits(s))
                bytes = FromHexString(s);
            else
                bytes.assign(s.begin(), s.end());
            bytes.resize((variable.bitLength + 7) / 8, 0);
            WriteBits(variable.dataPtr, variable.bitOffset, variable.bitLength, bytes.data());
            return;
        }

        default:
            // Silently ignore unsupported/opaque types, matching the original.
            return;
    }
}

} // namespace ecmqtt
