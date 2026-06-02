// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl {

// Channel assignments (ENet supports up to kChannelCount=2 per connection).
static constexpr uint8_t kNetChReliable = 0;
static constexpr uint8_t kNetChUnreliable = 1;

// Incremented whenever the wire format changes in a backward-incompatible way.
// Clients that receive a MsgHello with a different protocolVersion must disconnect.
static constexpr uint16_t kProtocolVersion = 1;

enum class MsgId : uint8_t {
    Hello = 0x00,         // server→client, reliable: first message sent on every new connection
    ConnectAck = 0x01,    // server→client, reliable: sent once on connect
    WorldSnapshot = 0x02, // server→client, unreliable: broadcast every sim tick
    ClientInput = 0x03,   // client→server, reliable: sent each frame
    WeatherState = 0x04,  // server→client, unreliable: broadcast every 10 ticks (~6 Hz); additive ID
    LanBeacon = 0x10,     // raw UDP broadcast — not sent over ENet; server→LAN presence packet
};

// All structs use #pragma pack(1) so the wire layout is identical on all platforms
// (no implicit padding). Always use std::memcpy to read/write these types from/to
// raw network buffers — direct pointer casting of unaligned wire data is UB.

#pragma pack(push, 1)

// Reliable, server→client, first message sent on every new connection.
// Client must check protocolVersion == kProtocolVersion and disconnect immediately on mismatch.
struct MsgHello {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Hello)};
    uint8_t _pad{0};
    uint16_t protocolVersion{kProtocolVersion};
}; // 4 bytes
static_assert(sizeof(MsgHello) == 4u, "MsgHello wire size changed");
static_assert(offsetof(MsgHello, protocolVersion) == 2u, "MsgHello::protocolVersion offset changed");

// Reliable, sent once on connect (after MsgHello).
// Followed by typeCount × MsgEntityTypeDef in the same packet.
struct MsgConnectAck {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectAck)};
    uint8_t tickRateHz{60};
    uint16_t typeCount{0};
    uint32_t assignedEntityIdx{0}; // entity slot assigned to this peer
    uint32_t assignedEntityGen{0}; // entity generation (0 = none assigned)
}; // 12 bytes
static_assert(sizeof(MsgConnectAck) == 12u, "MsgConnectAck wire size changed");
static_assert(offsetof(MsgConnectAck, tickRateHz) == 1u, "MsgConnectAck::tickRateHz offset changed");
static_assert(offsetof(MsgConnectAck, typeCount) == 2u, "MsgConnectAck::typeCount offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityIdx) == 4u, "MsgConnectAck::assignedEntityIdx offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityGen) == 8u, "MsgConnectAck::assignedEntityGen offset changed");

// Entity type definition appended after MsgConnectAck.
struct MsgEntityTypeDef {
    uint32_t typeIndex{0};
    char id[64]{};      // null-terminated type id, e.g. "builtin:debug-entity"
    char mesh[64]{};    // null-terminated mesh asset name; empty = builtin tetrahedron
    char dmgMesh[64]{}; // null-terminated damage mesh; empty = none
}; // 196 bytes
static_assert(sizeof(MsgEntityTypeDef) == 196u, "MsgEntityTypeDef wire size changed");
static_assert(offsetof(MsgEntityTypeDef, id) == 4u, "MsgEntityTypeDef::id offset changed");
static_assert(offsetof(MsgEntityTypeDef, mesh) == 68u, "MsgEntityTypeDef::mesh offset changed");
static_assert(offsetof(MsgEntityTypeDef, dmgMesh) == 132u, "MsgEntityTypeDef::dmgMesh offset changed");

// Unreliable, broadcast every sim tick.
// Followed by entityCount × MsgEntityEntry in the same packet.
struct MsgWorldSnapshotHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WorldSnapshot)};
    uint8_t protocolVersion{static_cast<uint8_t>(kProtocolVersion)};
    uint16_t entityCount{0};
    uint64_t tickIndex{0};
}; // 12 bytes
static_assert(sizeof(MsgWorldSnapshotHeader) == 12u, "MsgWorldSnapshotHeader wire size changed");
static_assert(offsetof(MsgWorldSnapshotHeader, entityCount) == 2u,
              "MsgWorldSnapshotHeader::entityCount offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, tickIndex) == 4u,
              "MsgWorldSnapshotHeader::tickIndex offset changed"); // misaligned: always use memcpy

// Per-entity snapshot entry appended after MsgWorldSnapshotHeader.
struct MsgEntityEntry {
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint32_t typeIndex{0};
    double pos[3]{}; // world position (m), XYZ — double for planet-scale precision
    float vel[3]{};  // world velocity (m/s) for dead-reckoning
    float ori[4]{};  // orientation quaternion: x, y, z, w (matches EntityTransform::quat)
    uint8_t damageLevel{0};
    uint8_t flags{0};    // bit 0 = playerOwned
    uint8_t throttle{0}; // [0–100] throttle_actual * 100; 0 for non-player entities
    uint8_t fuelPct{0};  // [0–100] fuel_kg / max_fuel * 100; 0 for non-player entities
}; // 68 bytes
static_assert(sizeof(MsgEntityEntry) == 68u, "MsgEntityEntry wire size changed");
static_assert(offsetof(MsgEntityEntry, typeIndex) == 8u, "MsgEntityEntry::typeIndex offset changed");
static_assert(
    offsetof(MsgEntityEntry, pos) == 12u,
    "MsgEntityEntry::pos offset changed"); // misaligned double[3]: always use memcpy; ARM64 SIGBUS on direct deref
static_assert(offsetof(MsgEntityEntry, vel) == 36u, "MsgEntityEntry::vel offset changed");
static_assert(offsetof(MsgEntityEntry, ori) == 48u, "MsgEntityEntry::ori offset changed");
static_assert(offsetof(MsgEntityEntry, damageLevel) == 64u, "MsgEntityEntry::damageLevel offset changed");
static_assert(offsetof(MsgEntityEntry, flags) == 65u, "MsgEntityEntry::flags offset changed");
static_assert(offsetof(MsgEntityEntry, throttle) == 66u, "MsgEntityEntry::throttle offset changed");
static_assert(offsetof(MsgEntityEntry, fuelPct) == 67u, "MsgEntityEntry::fuelPct offset changed");

// Reliable, client→server, sent each render frame.
struct MsgClientInput {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ClientInput)};
    uint8_t buttons{0}; // bit 0 = weaponTrigger, bit 1 = afterburner
    uint16_t protocolVersion{kProtocolVersion};
    uint32_t seqNum{0};    // client-incremented wrapping sequence counter
    uint64_t tickIndex{0}; // client's last-received server tick (reserved for lag compensation — see #142)
    float throttle{0.f};   // [0.0, 1.0]
    float elevator{0.f};   // [-1.0, +1.0] nose-up positive
    float aileron{0.f};    // [-1.0, +1.0] right-roll positive
    float rudder{0.f};     // [-1.0, +1.0] right-yaw positive
    float viewAxis[3]{};   // normalized look direction (world space)
}; // 1+1+2+4+8+4+4+4+4+12 = 44 bytes
static_assert(sizeof(MsgClientInput) == 44u, "MsgClientInput wire size changed");
static_assert(offsetof(MsgClientInput, seqNum) == 4u, "MsgClientInput::seqNum offset changed");
static_assert(offsetof(MsgClientInput, tickIndex) == 8u, "MsgClientInput::tickIndex offset changed");
static_assert(offsetof(MsgClientInput, throttle) == 16u, "MsgClientInput::throttle offset changed");
static_assert(offsetof(MsgClientInput, viewAxis) == 32u, "MsgClientInput::viewAxis offset changed");

// Unreliable, server→client, broadcast every 10 sim ticks (~6 Hz at 60 Hz).
// Old clients that don't recognise msgId 0x04 silently discard — no kProtocolVersion bump.
// timeOfDayTenths: encode timeOfDay as uint16 (hours * 10) to avoid float at offset 2 (ARM64).
struct MsgWeatherState {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WeatherState)};
    uint8_t preset{0};           // WeatherPreset cast to uint8_t
    uint16_t timeOfDayTenths{0}; // hours * 10; decode: / 10.f; range [0, 239]
    float fogDensity{0.f};
    float fogStartDist{5000.f};
    float windX{0.f}; // world-frame wind x (m/s), includes gust component
    float windZ{0.f}; // world-frame wind z (m/s), includes gust component
}; // 1+1+2+4+4+4+4 = 20 bytes
static_assert(sizeof(MsgWeatherState) == 20u, "MsgWeatherState wire size changed");
static_assert(offsetof(MsgWeatherState, preset) == 1u, "MsgWeatherState::preset offset changed");
static_assert(offsetof(MsgWeatherState, timeOfDayTenths) == 2u, "MsgWeatherState::timeOfDayTenths offset changed");
static_assert(offsetof(MsgWeatherState, fogDensity) == 4u, "MsgWeatherState::fogDensity offset changed");
static_assert(offsetof(MsgWeatherState, fogStartDist) == 8u, "MsgWeatherState::fogStartDist offset changed");
static_assert(offsetof(MsgWeatherState, windX) == 12u, "MsgWeatherState::windX offset changed");
static_assert(offsetof(MsgWeatherState, windZ) == 16u, "MsgWeatherState::windZ offset changed");

// Raw UDP presence broadcast sent by fl-server on 255.255.255.255:<port> (IPv4 broadcast) and
// [ff02::1]:<port> (IPv6 link-local multicast) every discoveryIntervalMs milliseconds.
// Not sent over ENet — must not be injected into an ENet connection.
// Clients receive this to populate a server list (issue #143).
struct MsgLanBeacon {
    uint8_t msgId{static_cast<uint8_t>(MsgId::LanBeacon)}; // offset 0
    uint8_t _pad{0};                                       // offset 1
    uint16_t protocolVersion{kProtocolVersion};            // offset 2
    uint16_t gamePort{4778};                               // offset 4
    uint8_t playerCount{0};                                // offset 6
    uint8_t maxPlayers{0};                                 // offset 7
    uint8_t gameModeFlags{0};                              // offset 8 — see kGameMode* constants
    uint8_t _pad2{0};                                      // offset 9
    char name[64]{};                                       // offset 10 — null-terminated server name
}; // 74 bytes
static_assert(sizeof(MsgLanBeacon) == 74u, "MsgLanBeacon wire size changed");
static_assert(offsetof(MsgLanBeacon, protocolVersion) == 2u, "MsgLanBeacon::protocolVersion offset changed");
static_assert(offsetof(MsgLanBeacon, gamePort) == 4u, "MsgLanBeacon::gamePort offset changed");
static_assert(offsetof(MsgLanBeacon, playerCount) == 6u, "MsgLanBeacon::playerCount offset changed");
static_assert(offsetof(MsgLanBeacon, maxPlayers) == 7u, "MsgLanBeacon::maxPlayers offset changed");
static_assert(offsetof(MsgLanBeacon, gameModeFlags) == 8u, "MsgLanBeacon::gameModeFlags offset changed");
static_assert(offsetof(MsgLanBeacon, name) == 10u, "MsgLanBeacon::name offset changed");

#pragma pack(pop)

// Bitmask constants for MsgLanBeacon::gameModeFlags.
// Not an enum class so they compose cleanly with | and & on the uint8_t wire field.
static constexpr uint8_t kGameModeCampaign = 0x01u;
static constexpr uint8_t kGameModeMission = 0x02u;
static constexpr uint8_t kGameModeSandbox = 0x04u;

} // namespace fl
