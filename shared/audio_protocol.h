#pragma once
#include <cstddef>
#include <cstdint>
#include "version.h"
#include "audio_core.h"
#include <cstring>

namespace lanaudio {
constexpr uint32_t Magic = 0x4c415544;
constexpr uint8_t Version = 2;
constexpr size_t HeaderBytes = 40;
#ifndef LANAUDIO_PACKET_FRAMES
#define LANAUDIO_PACKET_FRAMES 96
#endif
static_assert(LANAUDIO_PACKET_FRAMES == 96 || LANAUDIO_PACKET_FRAMES == 192 || LANAUDIO_PACKET_FRAMES == 240,
              "LANAUDIO_PACKET_FRAMES must be 96, 192, or 240");
constexpr size_t PacketFrames = LANAUDIO_PACKET_FRAMES;
constexpr size_t PacketBytes = HeaderBytes + PacketFrames * 2 * sizeof(float);
constexpr uint32_t sequenceFromTimestamp(uint64_t timestamp) { return uint32_t(timestamp / PacketFrames); }
inline uint16_t read16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
inline uint32_t read32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t read64(const uint8_t* p) { return uint64_t(read32(p)) << 32 | read32(p + 4); }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void put64(uint8_t* p, uint64_t v) { put32(p, uint32_t(v >> 32)); put32(p + 4, uint32_t(v)); }
struct AudioPacket { uint64_t timestamp = 0; std::array<Stereo, PacketFrames> samples{}; };
struct PacketInfo { uint32_t rate = 0, sequence = 0; uint64_t session = 0, timestamp = 0; };
inline bool parsePacket(const uint8_t* bytes, size_t length, PacketInfo& info) {
    if (length != PacketBytes || read32(bytes) != Magic || bytes[4] != Version || bytes[5] != 2 ||
        read16(bytes + 6) != HeaderBytes || read16(bytes + 12) != PacketFrames) return false;
    info = {read32(bytes + 8), read32(bytes + 16), read64(bytes + 20), read64(bytes + 28)};
    return info.rate >= 8000 && info.rate <= 192000 && info.session != 0 &&
        info.timestamp % PacketFrames == 0 && sequenceFromTimestamp(info.timestamp) == info.sequence;
}
inline AudioPacket decodePacket(const uint8_t* bytes, const PacketInfo& info) {
    AudioPacket packet;
    packet.timestamp = info.timestamp;
    static_assert(sizeof(Stereo) == 2 * sizeof(float));
    std::memcpy(packet.samples.data(), bytes + HeaderBytes, PacketFrames * sizeof(Stereo));
    for (auto& sample : packet.samples) {
        if (!std::isfinite(sample.left)) sample.left = 0;
        if (!std::isfinite(sample.right)) sample.right = 0;
    }
    return packet;
}
}
