#pragma once
#include "audio_protocol.h"
#include <string_view>

namespace lanaudio {
constexpr uint16_t DiscoveryPort = 30011;
constexpr uint8_t DiscoveryVersion = 1;
constexpr uint32_t DiscoveryQuery = 0x4c414451, DiscoveryReply = 0x4c414452;
constexpr size_t DiscoveryQueryBytes = 9, DiscoveryHeaderBytes = 15, DiscoveryNameBytes = 63;
inline bool parseDiscoveryQuery(const uint8_t* bytes, size_t length, uint32_t& nonce) {
    if (length != DiscoveryQueryBytes || read32(bytes) != DiscoveryQuery || bytes[4] != DiscoveryVersion) return false;
    nonce = read32(bytes + 5); return true;
}
inline size_t writeDiscoveryReply(uint8_t* bytes, uint32_t nonce, uint16_t port, std::string_view name) {
    const auto count = std::min(name.size(), DiscoveryNameBytes);
    put32(bytes, DiscoveryReply); bytes[4] = DiscoveryVersion; put32(bytes + 5, nonce);
    bytes[9] = Version; put16(bytes + 10, port); put16(bytes + 12, PacketFrames); bytes[14] = uint8_t(count);
    std::memcpy(bytes + DiscoveryHeaderBytes, name.data(), count);
    return DiscoveryHeaderBytes + count;
}
}
