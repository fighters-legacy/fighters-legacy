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
//   (c) a new TLV EXTENSION entry appended after the fixed struct section   -> no version bump;
//       receivers that do not call fl::readExtValue ignore extension bytes naturally (see
//       WireCodec.h); see ExtTag below for the defined extension registry.
//   (d) changing an EXISTING field's meaning/offset/size is breaking        -> bump kProtocolVersion.
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
    Hello = 0x00,              // server->client, reliable: first message sent on every new connection
    ConnectAck = 0x01,         // server->client, reliable: sent once on connect
    WorldSnapshot = 0x02,      // server->client, unreliable: broadcast every sim tick
    ClientInput = 0x03,        // client->server, unreliable: sent each render frame
    WeatherState = 0x04,       // server->client, unreliable: broadcast every 10 ticks (~6 Hz)
    ServerNotice = 0x05,       // server->client, reliable: shutdown countdown and operator notices
    AdminCommand = 0x06,       // client->server, reliable: operator-authenticated admin command
    AdminResponse = 0x07,      // server->client, reliable: result text from dispatched admin command
    Motd = 0x08,               // server->client, reliable: MOTD sent once on connect after ConnectAck
    ConnectRefusal = 0x09,     // server->client, reliable: rejection reason sent before disconnectPeer()
    AdminResponseChunk = 0x0A, // server->client, reliable: streaming chunk for long admin command output
    Heartbeat = 0x0B,          // client->server, unreliable: liveness signal when idle; carries tickIndex to
                               // refresh estimatedDelayTicks without a full MsgClientInput
    PeerDelay = 0x0C,          // server->client, unreliable: server's estimatedDelayTicks reply to MsgHeartbeat
    // 0x0D-0x0E reserved for future ENet message types; 0x0F reserved.
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
    float planetRadiusKm{0.f};     // planet sphere radius (km); Earth default = 6371
}; // 16 bytes, align 4 (multiple of alignof(MsgEntityTypeDef) so trailing records stay aligned)
static_assert(sizeof(MsgConnectAck) == 16u, "MsgConnectAck wire size changed");
static_assert(alignof(MsgConnectAck) == 4u, "MsgConnectAck alignment changed");
static_assert(offsetof(MsgConnectAck, typeCount) == 2u, "MsgConnectAck::typeCount offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityIdx) == 4u, "MsgConnectAck::assignedEntityIdx offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityGen) == 8u, "MsgConnectAck::assignedEntityGen offset changed");
static_assert(offsetof(MsgConnectAck, planetRadiusKm) == 12u, "MsgConnectAck::planetRadiusKm offset changed");

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

// Unreliable, unicast per-peer every sim tick.
// Followed by fullEntityCount x MsgEntityEntry records (new or baseline-tick entities), then
// updateCount x MsgEntityUpdate records (compact state for entities already known to the peer),
// then the TLV extension block. Sized to 16 (multiple of 8) so each trailing MsgEntityEntry
// stays 8-aligned (its pos[3] is double).
struct MsgWorldSnapshotHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WorldSnapshot)};
    uint8_t protocolVersion{static_cast<uint8_t>(kProtocolVersion)};
    uint16_t fullEntityCount{0}; // number of MsgEntityEntry records that follow (new / baseline)
    uint16_t updateCount{0};     // number of MsgEntityUpdate records after the full entries
    uint16_t _reserved{0};       // pad so tickIndex is 8-aligned
    uint64_t tickIndex{0};
}; // 16 bytes, align 8
static_assert(sizeof(MsgWorldSnapshotHeader) == 16u, "MsgWorldSnapshotHeader wire size changed");
static_assert(alignof(MsgWorldSnapshotHeader) == 8u, "MsgWorldSnapshotHeader alignment changed");
static_assert(offsetof(MsgWorldSnapshotHeader, fullEntityCount) == 2u,
              "MsgWorldSnapshotHeader::fullEntityCount offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, updateCount) == 4u,
              "MsgWorldSnapshotHeader::updateCount offset changed");
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

// Compact per-tick state for entities already known to the receiving peer. Sent in the update
// section of MsgWorldSnapshot (after fullEntityCount x MsgEntityEntry). Uses float positions
// (absolute world coords — precision ~1.6 cm at 200 km from origin, sufficient for rendering;
// authoritative physics always uses double on the server). Omits static fields (typeIndex) which
// the client caches from the last full MsgEntityEntry for this entity.
// entityGen is uint16_t (truncated from EntityId::generation uint32_t); 65535 respawns of the
// same pool slot per session is impossible in practice, so the truncation is safe.
struct MsgEntityUpdate {
    uint32_t entityIdx{0};      // @0 — which entity
    uint16_t entityGen{0};      // @4 — generation (truncated); mismatch vs cached → treat as new
    uint8_t damageLevel{0};     // @6
    uint8_t engineFailFlags{0}; // @7 — fl::kEngineFail* bitmask
    float pos[3]{};             // @8  — absolute world position (float, see precision note above)
    float vel[3]{};             // @20 — world-frame velocity (m/s)
    float ori[4]{};             // @32 — orientation quaternion x,y,z,w
    uint8_t throttle{0};        // @48 — [0-100]
    uint8_t fuelPct{0};         // @49 — [0-100]
    uint8_t abEngaged{0};       // @50 — 1 when afterburner lit
    uint8_t flags{0};           // @51 — bit 0 = playerOwned (same as MsgEntityEntry::flags)
}; // 52 bytes, align 4
static_assert(sizeof(MsgEntityUpdate) == 52u, "MsgEntityUpdate wire size changed");
static_assert(alignof(MsgEntityUpdate) == 4u, "MsgEntityUpdate alignment changed");
static_assert(sizeof(MsgEntityUpdate) % alignof(MsgEntityUpdate) == 0u, "MsgEntityUpdate not record-aligned");
static_assert(offsetof(MsgEntityUpdate, entityIdx) == 0u, "MsgEntityUpdate::entityIdx offset changed");
static_assert(offsetof(MsgEntityUpdate, entityGen) == 4u, "MsgEntityUpdate::entityGen offset changed");
static_assert(offsetof(MsgEntityUpdate, pos) == 8u, "MsgEntityUpdate::pos offset changed");
static_assert(offsetof(MsgEntityUpdate, vel) == 20u, "MsgEntityUpdate::vel offset changed");
static_assert(offsetof(MsgEntityUpdate, ori) == 32u, "MsgEntityUpdate::ori offset changed");
static_assert(offsetof(MsgEntityUpdate, throttle) == 48u, "MsgEntityUpdate::throttle offset changed");

// Unreliable, client->server, sent each render frame. Padded to 48 (multiple of 8 for tickIndex).
struct MsgClientInput {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ClientInput)};
    uint8_t buttons{0}; // bit 0 = weaponTrigger, bit 1 = afterburner
    uint16_t protocolVersion{kProtocolVersion};
    uint32_t seqNum{0};    // monotonically increasing; server discards packets not newer than last accepted
    uint64_t tickIndex{0}; // server's tickIndex from last received WorldSnapshot; server uses delta for delay estimate
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

// Reliable, client->server. Carries a correlation ID, operator token, and command string.
// Server authenticates via constant-time token comparison before dispatching.
// reqId is a client-generated correlation ID echoed in every response packet for this command.
struct MsgAdminCommand {
    uint8_t msgId{static_cast<uint8_t>(MsgId::AdminCommand)};
    uint8_t reserved{0};
    uint16_t reqId{0};  // client-generated correlation ID; echoed in response
    char token[30]{};   // null-terminated operator password; 29 usable chars
    char command[94]{}; // null-terminated command text; 93 usable chars
}; // 128 bytes, align 2
static_assert(sizeof(MsgAdminCommand) == 128u, "MsgAdminCommand wire size changed");
static_assert(alignof(MsgAdminCommand) == 2u, "MsgAdminCommand alignment changed");
static_assert(offsetof(MsgAdminCommand, reqId) == 2u, "MsgAdminCommand::reqId offset changed");
static_assert(offsetof(MsgAdminCommand, token) == 4u, "MsgAdminCommand::token offset changed");
static_assert(offsetof(MsgAdminCommand, command) == 34u, "MsgAdminCommand::command offset changed");

// Reliable, server->client unicast. Fast path for results <= kAdminResponseFastPathMax chars.
// Longer results are streamed as MsgAdminResponseChunk (0x0A) packets instead.
// Empty text (text[0] == '\0') means the command was queued asynchronously; clients may ignore.
// reqId echoes the triggering MsgAdminCommand::reqId for request/response correlation.
struct MsgAdminResponse {
    uint8_t msgId{static_cast<uint8_t>(MsgId::AdminResponse)};
    uint8_t reserved{0};
    uint16_t reqId{0}; // echoed from triggering MsgAdminCommand::reqId
    char text[124]{};  // null-terminated response; 123 usable chars
}; // 128 bytes, align 2
static_assert(sizeof(MsgAdminResponse) == 128u, "MsgAdminResponse wire size changed");
static_assert(alignof(MsgAdminResponse) == 2u, "MsgAdminResponse alignment changed");
static_assert(offsetof(MsgAdminResponse, reqId) == 2u, "MsgAdminResponse::reqId offset changed");
static_assert(offsetof(MsgAdminResponse, text) == 4u, "MsgAdminResponse::text offset changed");

// Reliable, server->client unicast. Streaming path for results > kAdminResponseFastPathMax chars.
// Old clients silently discard 0x0A (additive message pattern). ENet reliable channel guarantees
// in-order delivery, so seqNum is diagnostic only. kChunkFlagEnd (bit 0 of flags) marks the final
// chunk; the client appends all body strings and prints once the end chunk arrives.
// reqId echoes the triggering MsgAdminCommand::reqId.
struct MsgAdminResponseChunk {
    uint8_t msgId{static_cast<uint8_t>(MsgId::AdminResponseChunk)};
    uint8_t flags{0};   // bit 0 = kChunkFlagEnd (set on the final chunk of a response)
    uint16_t reqId{0};  // echoed from triggering MsgAdminCommand::reqId
    uint16_t seqNum{0}; // 0-based chunk index; diagnostic only (ENet guarantees ordering)
    char body[506]{};   // null-terminated chunk body; 505 usable chars
}; // 512 bytes, align 2
static_assert(sizeof(MsgAdminResponseChunk) == 512u, "MsgAdminResponseChunk wire size changed");
static_assert(alignof(MsgAdminResponseChunk) == 2u, "MsgAdminResponseChunk alignment changed");
static_assert(offsetof(MsgAdminResponseChunk, flags) == 1u, "MsgAdminResponseChunk::flags offset changed");
static_assert(offsetof(MsgAdminResponseChunk, reqId) == 2u, "MsgAdminResponseChunk::reqId offset changed");
static_assert(offsetof(MsgAdminResponseChunk, seqNum) == 4u, "MsgAdminResponseChunk::seqNum offset changed");
static_assert(offsetof(MsgAdminResponseChunk, body) == 6u, "MsgAdminResponseChunk::body offset changed");

// Flags for MsgAdminResponseChunk::flags.
static constexpr uint8_t kChunkFlagEnd = 0x01u; // set on the final chunk of a streamed response

// Thresholds derived from struct field sizes (defined after all structs so sizeof is valid).
// Results <= kAdminResponseFastPathMax bytes use the MsgAdminResponse fast path (single packet).
// Each MsgAdminResponseChunk carries at most kAdminChunkPayload usable bytes.
static constexpr std::size_t kAdminResponseFastPathMax = sizeof(MsgAdminResponse::text) - 1u; // 123
static constexpr std::size_t kAdminChunkPayload = sizeof(MsgAdminResponseChunk::body) - 1u;   // 505

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

// Unreliable, client->server. Liveness heartbeat for idle clients (e.g., future spectator mode)
// that are not sending MsgClientInput. tickIndex carries the last received WorldSnapshot tick so the
// server can update estimatedDelayTicks. Only send after receiving at least one WorldSnapshot
// (tickIndex == 0 would produce a bogus server-side delay estimate).
struct MsgHeartbeat {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Heartbeat)};
    uint8_t reserved[7]{}; // pad so tickIndex is 8-aligned
    uint64_t tickIndex{0}; // last received WorldSnapshot tickIndex (same semantic as MsgClientInput)
}; // 16 bytes, align 8
static_assert(sizeof(MsgHeartbeat) == 16u, "MsgHeartbeat wire size changed");
static_assert(alignof(MsgHeartbeat) == 8u, "MsgHeartbeat alignment changed");
static_assert(offsetof(MsgHeartbeat, tickIndex) == 8u, "MsgHeartbeat::tickIndex offset changed");

// Unreliable, server->client unicast. Reply to MsgHeartbeat; delivers the server's current
// estimatedDelayTicks for this peer so the client can display "Ping: N ms".
// Arrives ~1 Hz (matching the heartbeat rate). Convert to ms: delayTicks * 1000 / 60.
// delayTicks == 0 means the server has not accepted any tickIndex yet; clients must ignore it.
struct MsgPeerDelay {
    uint8_t msgId{static_cast<uint8_t>(MsgId::PeerDelay)};
    uint8_t reserved{0};
    uint16_t delayTicks{0}; // estimatedDelayTicks capped at 65535 (~18 min at 60 Hz)
}; // 4 bytes, align 2
static_assert(sizeof(MsgPeerDelay) == 4u, "MsgPeerDelay wire size changed");
static_assert(alignof(MsgPeerDelay) == 2u, "MsgPeerDelay alignment changed");
static_assert(offsetof(MsgPeerDelay, delayTicks) == 2u, "MsgPeerDelay::delayTicks offset changed");

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

// Extension tag registry for TLV blocks appended after fixed message structs (see WireCodec.h).
// Wire format per entry: [tag: uint16_t LE][len: uint16_t LE][data: len bytes].
// Senders include any subset; receivers skip unknown tags via their len field.
// Range layout:
//   0x0100–0x01FF  MsgWorldSnapshot extensions (appended after entity record array)
//   0x0200–0x02FF  MsgConnectAck extensions (reserved for future use)
//   0x0300–0x03FF  MsgClientInput extensions (reserved for future use)
//   0x0400–0x04FF  MsgWeatherState extensions (reserved for future use)
//   Values outside defined ranges are reserved and must not be sent.
enum class ExtTag : uint16_t {
    SnapshotPeerCount = 0x0100, // uint16_t: active connected peer count at snapshot time
};

} // namespace fl
