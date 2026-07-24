// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputTraceFormat — on-disk layout for a recorded MsgClientInput stream ("FLIT" trace).
//
// A trace captures the accepted (post-validation) control inputs of one peer during a real
// server session so the bot_swarm load harness can replay real player behaviour at scale
// (issue #560), and so the Phase 4 replay epic (#588) has a versioned server-side input stream
// to build on rather than fork. The format is little-endian and self-describing:
//
//   Header (10 bytes):  magic "FLIT" (4) | version u16 | tickRate u32
//   Record (32 bytes):  serverTick u64 | throttle f32 | elevator f32 | aileron f32 |
//                       rudder f32 | buttons u32 | flaps u8 | speedbrake u8 |
//                       artButtons u8 | reserved u8
//
// Records follow the header back-to-back; the record count is (fileSize - 10) / 28. All
// integers/floats are stored little-endian regardless of host endianness (a trace produced on
// one machine replays on any), so the codec serialises bytes explicitly rather than memcpy'ing
// native words. The five control fields map 1:1 onto the harness's BotControl and onto
// MsgClientInput's flight-control fields.

#include <cstdint>
#include <cstring>
#include <vector>

namespace fl {

constexpr char kInputTraceMagic[4] = {'F', 'L', 'I', 'T'};
constexpr uint16_t kInputTraceVersion = 1;
constexpr std::size_t kInputTraceHeaderBytes = 10;
constexpr std::size_t kInputTraceRecordBytes = 32;

// One captured input sample. serverTick is the authoritative tick at which the server accepted
// the input; the four axes are the sanitized [0,1]/[-1,1] actuator values; buttons is the raw
// MsgClientInput button bitmask (bit 0 = weapon, bit 1 = afterburner).
//
// The articulation commands (#843) are recorded too, because gear and flap POSITION is drag: a
// replay that dropped them would fly a different aeroplane from the session it recorded, and a
// determinism replay that silently differs is worse than none. `kInputTraceVersion` deliberately
// stays 1 — producer and consumer live in this repo and land together, so a version bump would be a
// compatibility promise to a party that does not exist.
struct InputTraceRecord {
    uint64_t serverTick{0};
    float throttle{0.f};
    float elevator{0.f};
    float aileron{0.f};
    float rudder{0.f};
    uint32_t buttons{0};
    uint8_t flaps{0};      // commanded flap position, 0..255 => 0..1
    uint8_t speedbrake{0}; // commanded speed-brake, 0..255 => 0..1
    uint8_t artButtons{0}; // kArtButton* bitmask: gear / hook / canopy
    uint8_t reserved{0};   // explicit pad to a 32-byte record
};

namespace detail {

inline void putU16LE(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
inline void putU32LE(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
inline void putU64LE(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
inline void putF32LE(std::vector<uint8_t>& b, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32LE(b, bits);
}

inline uint16_t getU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t getU32LE(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
inline uint64_t getU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
inline float getF32LE(const uint8_t* p) {
    uint32_t bits = getU32LE(p);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

} // namespace detail

} // namespace fl
