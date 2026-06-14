// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace fl {

// Channel assignments (ENet supports up to kChannelCount=2 per connection).
static constexpr uint8_t kNetChReliable = 0;
static constexpr uint8_t kNetChUnreliable = 1;
//
// ---------------------------------------------------------------------------------------------
// Wire-format compatibility model
// ---------------------------------------------------------------------------------------------
// During primary development the wire format may change freely: the game client always spawns the
// same-tree fl-server (single-player) and multiplayer is dev-only, so client and server are always
// built together and kProtocolVersion stays at 1. The version field exists for the Phase 6 public
// release, when the format FREEZES and these rules begin to bind:
//   (a) a new MESSAGE TYPE gets a new MsgId; old peers discard unknown ids  -> no version bump;
//   (b) a new TRAILING field appended to an existing struct is additive     -> no version bump;
//   (c) changing an EXISTING field's meaning/offset/size is breaking        -> bump kProtocolVersion.
//
// Layout rules (enforced by the static_asserts below):
//   * Wire structs are NOT packed. Fields are ordered large->small and padded to natural alignment
//     with explicit `reserved` fields, so every field lands on its natural offset and the compiler
//     inserts no implicit padding. Using only fixed-width types (no long/bool/pointers/enums) makes
//     the layout byte-identical across MSVC / GCC / Clang on every supported ABI.
//   * Array-message headers (MsgConnectAck, MsgWorldSnapshotHeader) and their record types
//     (MsgEntityTypeDef, MsgEntityEntry) are sized to multiples of the record alignment so the i-th
//     record stays naturally aligned. ENet/std::vector buffer bases are >= max_align_t, so a
//     received buffer can be read in place via fl::viewMsg (see WireCodec.h) without a copy.
//   * fl::readMsg (memcpy) remains the portable default; fl::viewMsg is the zero-copy fast path.
// ---------------------------------------------------------------------------------------------

// Incremented only at the Phase 6 public release when the wire format freezes (see compatibility
// model above). Stays at 1 throughout primary development. Clients that receive a MsgHello with a
// different protocolVersion must disconnect.
static constexpr uint16_t kProtocolVersion = 1;

// Server-enforced maximum byte length of the MsgMotd text payload (NUL terminator excluded).
// Client enforces the same cap on receive to guard against oversized packets.
static constexpr std::size_t kMaxMotdBytes = 65535;

enum class MsgId : uint8_t {
    Hello = 0x00,          // server->client, reliable: first message sent on every new connection
    ConnectAck = 0x01,     // server->client, reliable: sent once on connect
    WorldSnapshot = 0x02,  // server->client, unreliable: broadcast every sim tick
    ClientInput = 0x03,    // client->server, reliable: sent each frame
    WeatherState = 0x04,   // server->client, unreliable: broadcast every 10 ticks (~6 Hz)
    ServerNotice = 0x05,   // server->client, reliable: shutdown countdown and operator notices
    AdminCommand = 0x06,   // client->server, reliable: operator-authenticated admin command
    AdminResponse = 0x07,  // server->client, reliable: result text from dispatched admin command
    Motd = 0x08,           // server->client, reliable: MOTD sent once on connect after ConnectAck
    ConnectRefusal = 0x09, // server->client, reliable: rejection reason sent before disconnectPeer()
    // 0x0A-0x0E reserved for future ENet message types; 0x0F reserved.
    LanBeacon = 0x10, // raw UDP broadcast - NOT sent over ENet; 0x10+ reserved for non-ENet ids.
};

// Machine-readable reason carried in MsgConnectRefusal::code, alongside the human-readable text.
// Lets the client map a rejection to a localized string without parsing the English text.
enum class ConnectRefusalCode : uint8_t {
    Generic = 0,
    Banned = 1,
    AccessDenied = 2, // allowlist miss or admin-auth lockout
    RateLimited = 3,
    TooManyConnections = 4, // per-IP concurrent connection cap
    AdminLockout = 5,
};

// All structs below are deliberately UNPACKED and laid out for natural alignment (see compatibility
// model). Always read them out of a raw buffer with fl::readMsg / fl::viewMsg (WireCodec.h); a direct
// pointer cast of an unknown buffer is only valid through viewMsg's alignment guard.

// Reliable, server->client, first message sent on every new connection.
// Client must check protocolVersion == kProtocolVersion and disconnect immediately on mismatch.
struct MsgHello {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Hello)};
    uint8_t reserved{0};
    uint16_t protocolVersion{kProtocolVersion};
}; // 4 bytes, align 2
static_assert(sizeof(MsgHello) == 4u, "MsgHello wire size changed");
static_assert(alignof(MsgHello) == 2u, "MsgHello alignment changed");
static_assert(offsetof(MsgHello, protocolVersion) == 2u, "MsgHello::protocolVersion offset changed");

// Reliable, sent once on connect (after MsgHello).
// Followed by typeCount x MsgEntityTypeDef in the same packet.
struct MsgConnectAck {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectAck)};
    uint8_t tickRateHz{60};
    uint16_t typeCount{0};
    uint32_t assignedEntityIdx{0}; // entity slot assigned to this peer
    uint32_t assignedEntityGen{0}; // entity generation (0 = none assigned)
}; // 12 bytes, align 4 (multiple of alignof(MsgEntityTypeDef) so trailing records stay aligned)
static_assert(sizeof(MsgConnectAck) == 12u, "MsgConnectAck wire size changed");
static_assert(alignof(MsgConnectAck) == 4u, "MsgConnectAck alignment changed");
static_assert(offsetof(MsgConnectAck, typeCount) == 2u, "MsgConnectAck::typeCount offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityIdx) == 4u, "MsgConnectAck::assignedEntityIdx offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityGen) == 8u, "MsgConnectAck::assignedEntityGen offset changed");

// Entity type definition record appended after MsgConnectAck.
struct MsgEntityTypeDef {
    uint32_t typeIndex{0};
    char id[64]{};      // null-terminated type id, e.g. "builtin:debug-entity"
    char mesh[64]{};    // null-terminated mesh asset name; empty = builtin tetrahedron
    char dmgMesh[64]{}; // null-terminated damage mesh; empty = none
}; // 196 bytes, align 4
static_assert(sizeof(MsgEntityTypeDef) == 196u, "MsgEntityTypeDef wire size changed");
static_assert(alignof(MsgEntityTypeDef) == 4u, "MsgEntityTypeDef alignment changed");
static_assert(sizeof(MsgEntityTypeDef) % alignof(MsgEntityTypeDef) == 0u, "MsgEntityTypeDef not record-aligned");
static_assert(offsetof(MsgEntityTypeDef, id) == 4u, "MsgEntityTypeDef::id offset changed");
static_assert(offsetof(MsgEntityTypeDef, mesh) == 68u, "MsgEntityTypeDef::mesh offset changed");
static_assert(offsetof(MsgEntityTypeDef, dmgMesh) == 132u, "MsgEntityTypeDef::dmgMesh offset changed");

// Unreliable, broadcast every sim tick.
// Followed by entityCount x MsgEntityEntry in the same packet. Sized to 16 (multiple of 8) so each
// trailing MsgEntityEntry stays 8-aligned (its pos[3] is double).
struct MsgWorldSnapshotHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WorldSnapshot)};
    uint8_t protocolVersion{static_cast<uint8_t>(kProtocolVersion)};
    uint16_t entityCount{0};
    uint32_t reserved{0}; // pad so tickIndex is 8-aligned
    uint64_t tickIndex{0};
}; // 16 bytes, align 8
static_assert(sizeof(MsgWorldSnapshotHeader) == 16u, "MsgWorldSnapshotHeader wire size changed");
static_assert(alignof(MsgWorldSnapshotHeader) == 8u, "MsgWorldSnapshotHeader alignment changed");
static_assert(offsetof(MsgWorldSnapshotHeader, entityCount) == 2u,
              "MsgWorldSnapshotHeader::entityCount offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, tickIndex) == 8u, "MsgWorldSnapshotHeader::tickIndex offset changed");

// Per-entity snapshot record appended after MsgWorldSnapshotHeader. Laid out large->small: the
// double pos[3] is first (8-aligned), and the struct is padded to 72 (multiple of 8) so record i at
// header(16) + i*72 stays 8-aligned.
struct MsgEntityEntry {
    double pos[3]{}; // world position (m), XYZ - double for planet-scale precision
    float vel[3]{};  // world velocity (m/s) for dead-reckoning
    float ori[4]{};  // orientation quaternion: x, y, z, w (matches EntityTransform::quat)
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint32_t typeIndex{0};
    uint8_t damageLevel{0};
    uint8_t flags{0};           // bit 0 = playerOwned
    uint8_t throttle{0};        // [0-100] throttle_actual * 100; 0 for non-player entities
    uint8_t fuelPct{0};         // [0-100] fuel_kg / max_fuel * 100; 0 for non-player entities
    uint8_t abEngaged{0};       // 1 when afterburner physically lit (FlightState::ab_engaged)
    uint8_t engineFailFlags{0}; // fl::kEngineFail* bitmask
    uint8_t reserved[2]{};      // pad to 72 (multiple of 8)
}; // 72 bytes, align 8
static_assert(sizeof(MsgEntityEntry) == 72u, "MsgEntityEntry wire size changed");
static_assert(alignof(MsgEntityEntry) == 8u, "MsgEntityEntry alignment changed");
static_assert(sizeof(MsgEntityEntry) % alignof(MsgEntityEntry) == 0u, "MsgEntityEntry not record-aligned");
static_assert(offsetof(MsgEntityEntry, pos) == 0u, "MsgEntityEntry::pos offset changed");
static_assert(offsetof(MsgEntityEntry, vel) == 24u, "MsgEntityEntry::vel offset changed");
static_assert(offsetof(MsgEntityEntry, ori) == 36u, "MsgEntityEntry::ori offset changed");
static_assert(offsetof(MsgEntityEntry, entityIdx) == 52u, "MsgEntityEntry::entityIdx offset changed");
static_assert(offsetof(MsgEntityEntry, typeIndex) == 60u, "MsgEntityEntry::typeIndex offset changed");
static_assert(offsetof(MsgEntityEntry, damageLevel) == 64u, "MsgEntityEntry::damageLevel offset changed");
static_assert(offsetof(MsgEntityEntry, engineFailFlags) == 69u, "MsgEntityEntry::engineFailFlags offset changed");

// Reliable, client->server, sent each render frame. Padded to 48 (multiple of 8 for tickIndex).
struct MsgClientInput {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ClientInput)};
    uint8_t buttons{0}; // bit 0 = weaponTrigger, bit 1 = afterburner
    uint16_t protocolVersion{kProtocolVersion};
    uint32_t seqNum{0};    // client-incremented wrapping sequence counter
    uint64_t tickIndex{0}; // client's last-received server tick (reserved for lag compensation)
    float throttle{0.f};   // [0.0, 1.0]
    float elevator{0.f};   // [-1.0, +1.0] nose-up positive
    float aileron{0.f};    // [-1.0, +1.0] right-roll positive
    float rudder{0.f};     // [-1.0, +1.0] right-yaw positive
    float viewAxis[3]{};   // normalized look direction (world space)
    uint8_t reserved[4]{}; // pad to 48
}; // 48 bytes, align 8
static_assert(sizeof(MsgClientInput) == 48u, "MsgClientInput wire size changed");
static_assert(alignof(MsgClientInput) == 8u, "MsgClientInput alignment changed");
static_assert(offsetof(MsgClientInput, seqNum) == 4u, "MsgClientInput::seqNum offset changed");
static_assert(offsetof(MsgClientInput, tickIndex) == 8u, "MsgClientInput::tickIndex offset changed");
static_assert(offsetof(MsgClientInput, throttle) == 16u, "MsgClientInput::throttle offset changed");
static_assert(offsetof(MsgClientInput, viewAxis) == 32u, "MsgClientInput::viewAxis offset changed");

// Unreliable, server->client, broadcast every 10 sim ticks (~6 Hz at 60 Hz).
// timeOfDayTenths: encode timeOfDay as uint16 (hours * 10) to keep it 2-aligned.
struct MsgWeatherState {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WeatherState)};
    uint8_t preset{0};           // WeatherPreset cast to uint8_t
    uint16_t timeOfDayTenths{0}; // hours * 10; decode: / 10.f; range [0, 239]
    float fogDensity{0.f};
    float fogStartDist{5000.f};
    float windX{0.f}; // world-frame wind x (m/s), includes gust component
    float windZ{0.f}; // world-frame wind z (m/s), includes gust component
}; // 20 bytes, align 4
static_assert(sizeof(MsgWeatherState) == 20u, "MsgWeatherState wire size changed");
static_assert(alignof(MsgWeatherState) == 4u, "MsgWeatherState alignment changed");
static_assert(offsetof(MsgWeatherState, timeOfDayTenths) == 2u, "MsgWeatherState::timeOfDayTenths offset changed");
static_assert(offsetof(MsgWeatherState, fogDensity) == 4u, "MsgWeatherState::fogDensity offset changed");
static_assert(offsetof(MsgWeatherState, windX) == 12u, "MsgWeatherState::windX offset changed");
static_assert(offsetof(MsgWeatherState, windZ) == 16u, "MsgWeatherState::windZ offset changed");

// Reliable, server->client. Sent at each countdown interval and at T=0 before graceful disconnect.
// secondsRemaining == 0 means shutdown is imminent (final notice).
// text is null-terminated UTF-8; guaranteed within 60 bytes by the server.
struct MsgServerNotice {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ServerNotice)};
    uint8_t reserved{0};
    uint16_t secondsRemaining{0};
    char text[60]{};
}; // 64 bytes, align 2
static_assert(sizeof(MsgServerNotice) == 64u, "MsgServerNotice wire size changed");
static_assert(alignof(MsgServerNotice) == 2u, "MsgServerNotice alignment changed");
static_assert(offsetof(MsgServerNotice, secondsRemaining) == 2u, "MsgServerNotice::secondsRemaining offset changed");
static_assert(offsetof(MsgServerNotice, text) == 4u, "MsgServerNotice::text offset changed");

// Reliable, client->server. Carries an operator token + command string.
// Server authenticates via constant-time token comparison before dispatching.
struct MsgAdminCommand {
    uint8_t msgId{static_cast<uint8_t>(MsgId::AdminCommand)};
    uint8_t reserved{0};
    char token[30]{};   // null-terminated operator password; 29 usable chars
    char command[96]{}; // null-terminated command text; 95 usable chars
}; // 128 bytes, align 1
static_assert(sizeof(MsgAdminCommand) == 128u, "MsgAdminCommand wire size changed");
static_assert(offsetof(MsgAdminCommand, token) == 2u, "MsgAdminCommand::token offset changed");
static_assert(offsetof(MsgAdminCommand, command) == 32u, "MsgAdminCommand::command offset changed");

// Reliable, server->client unicast. Carries the result text of a dispatched admin command.
// Empty text (text[0] == '\0') means the command was queued asynchronously; clients may ignore.
struct MsgAdminResponse {
    uint8_t msgId{static_cast<uint8_t>(MsgId::AdminResponse)};
    uint8_t reserved{0};
    char text[126]{}; // null-terminated response; 125 usable chars
}; // 128 bytes, align 1
static_assert(sizeof(MsgAdminResponse) == 128u, "MsgAdminResponse wire size changed");
static_assert(offsetof(MsgAdminResponse, text) == 2u, "MsgAdminResponse::text offset changed");

// Fixed-size header for MsgMotd (0x08). The null-terminated text payload follows at offset 4.
// Reliable, server->client unicast; sent once after MsgConnectAck when [server].motd non-empty.
// displaySeconds: server-requested banner duration (seconds); 0 = use client default.
struct MsgMotdHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Motd)};
    uint8_t reserved{0};
    uint16_t displaySeconds{0}; // 0 = client default
}; // 4 bytes, align 2; char text[] + NUL follow at offset 4
static_assert(sizeof(MsgMotdHeader) == 4u, "MsgMotdHeader wire size changed");
static_assert(alignof(MsgMotdHeader) == 2u, "MsgMotdHeader alignment changed");
static_assert(offsetof(MsgMotdHeader, displaySeconds) == 2u, "MsgMotdHeader::displaySeconds offset changed");

// Reliable, server->client unicast. Sent immediately before disconnectPeer() on every onConnect
// rejection (ban, allowlist, rate-limit, per-IP connection limit, admin auth lockout).
// reason is null-terminated UTF-8; guaranteed within 61 bytes by the server.
struct MsgConnectRefusal {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectRefusal)};
    uint8_t code{0};   // ConnectRefusalCode; machine-readable reason paired with the text
    char reason[62]{}; // null-terminated UTF-8; 61 usable chars
}; // 64 bytes, align 1
static_assert(sizeof(MsgConnectRefusal) == 64u, "MsgConnectRefusal wire size changed");
static_assert(offsetof(MsgConnectRefusal, code) == 1u, "MsgConnectRefusal::code offset changed");
static_assert(offsetof(MsgConnectRefusal, reason) == 2u, "MsgConnectRefusal::reason offset changed");

// Raw UDP presence broadcast sent by fl-server on 255.255.255.255:<port> (IPv4 broadcast) and
// [ff02::1]:<port> (IPv6 link-local multicast) every discoveryIntervalMs milliseconds.
// Not sent over ENet - must not be injected into an ENet connection.
struct MsgLanBeacon {
    uint8_t msgId{static_cast<uint8_t>(MsgId::LanBeacon)};
    uint8_t reserved{0};
    uint16_t protocolVersion{kProtocolVersion};
    uint16_t gamePort{4778};
    uint8_t playerCount{0};
    uint8_t maxPlayers{0};
    uint8_t gameModeFlags{0}; // see kGameMode* constants
    uint8_t reserved2{0};
    char name[64]{}; // null-terminated server name
}; // 74 bytes, align 2
static_assert(sizeof(MsgLanBeacon) == 74u, "MsgLanBeacon wire size changed");
static_assert(alignof(MsgLanBeacon) == 2u, "MsgLanBeacon alignment changed");
static_assert(offsetof(MsgLanBeacon, protocolVersion) == 2u, "MsgLanBeacon::protocolVersion offset changed");
static_assert(offsetof(MsgLanBeacon, gamePort) == 4u, "MsgLanBeacon::gamePort offset changed");
static_assert(offsetof(MsgLanBeacon, playerCount) == 6u, "MsgLanBeacon::playerCount offset changed");
static_assert(offsetof(MsgLanBeacon, maxPlayers) == 7u, "MsgLanBeacon::maxPlayers offset changed");
static_assert(offsetof(MsgLanBeacon, gameModeFlags) == 8u, "MsgLanBeacon::gameModeFlags offset changed");
static_assert(offsetof(MsgLanBeacon, name) == 10u, "MsgLanBeacon::name offset changed");

// Bitmask constants for MsgLanBeacon::gameModeFlags.
static constexpr uint8_t kGameModeCampaign = 0x01u;
static constexpr uint8_t kGameModeMission = 0x02u;
static constexpr uint8_t kGameModeSandbox = 0x04u;

} // namespace fl
