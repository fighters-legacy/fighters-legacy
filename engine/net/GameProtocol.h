// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Channel assignments (ENet supports up to kChannelCount=2 per connection).
static constexpr uint8_t kNetChReliable = 0;
static constexpr uint8_t kNetChUnreliable = 1;

enum class MsgId : uint8_t {
    ConnectAck = 0x01,    // server→client, reliable: sent once on connect
    WorldSnapshot = 0x02, // server→client, unreliable: broadcast every sim tick
};

// All structs use #pragma pack(1) so the wire layout is identical on all platforms
// (no implicit padding). Always use std::memcpy to read/write these types from/to
// raw network buffers — direct pointer casting of unaligned wire data is UB.

#pragma pack(push, 1)

// Reliable, sent once on connect.
// Followed by typeCount × MsgEntityTypeDef in the same packet.
struct MsgConnectAck {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectAck)};
    uint8_t tickRateHz{60};
    uint16_t typeCount{0};
}; // 4 bytes

// Entity type definition appended after MsgConnectAck.
struct MsgEntityTypeDef {
    uint32_t typeIndex{0};
    char id[64]{};      // null-terminated type id, e.g. "builtin:debug-entity"
    char mesh[64]{};    // null-terminated mesh asset name; empty = builtin tetrahedron
    char dmgMesh[64]{}; // null-terminated damage mesh; empty = none
}; // 196 bytes

// Unreliable, broadcast every sim tick.
// Followed by entityCount × MsgEntityEntry in the same packet.
struct MsgWorldSnapshotHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WorldSnapshot)};
    uint8_t _pad{0};
    uint16_t entityCount{0};
    uint64_t tickIndex{0};
}; // 12 bytes

// Per-entity snapshot entry appended after MsgWorldSnapshotHeader.
struct MsgEntityEntry {
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint32_t typeIndex{0};
    float pos[3]{}; // world position (m), XYZ
    float vel[3]{}; // world velocity (m/s) for dead-reckoning
    float ori[4]{}; // orientation quaternion: x, y, z, w (matches EntityTransform::quat)
    uint8_t damageLevel{0};
    uint8_t flags{0}; // bit 0 = playerOwned
    uint8_t _pad[2]{};
}; // 56 bytes

#pragma pack(pop)

} // namespace fl
