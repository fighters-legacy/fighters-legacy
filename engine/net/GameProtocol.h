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
    WingmanCommand = 0x0D,     // client->server, reliable: order this peer's wingman(s) (#610)
    WingmanAck = 0x0E,         // server->client, reliable: outcome of a wingman order, and the
                               // unsolicited flight check-in sent once after ConnectAck
    CombatEvent = 0x0F,        // server->client, reliable: kill feed + per-peer combat stats (#626).
                               // Multiplexed record stream (CombatEventType) rather than one message per
                               // event: every future gameplay event extends the record vocabulary, not
                               // the id space.
    FactionDef = 0x10,         // server->client, reliable: faction index -> id/name table, sent once after
                               // ConnectAck (#860). Lets the client label an entity's faction (from the
                               // snapshot's per-entity factionIndex) and, later, colour friend/foe.
    ConnectRequest = 0x11,     // client->server, reliable: FIRST packet the client sends on connect
                               // (#853). Carries the role, requested entity type, and mounted-pack
                               // manifest the client asks to join with; the server replies ConnectAck
                               // (granted role) or ConnectRefusal. See MsgConnectRequest below.
    LanBeacon = 0x20,          // raw UDP broadcast - NOT sent over ENet; 0x20+ reserved for non-ENet ids.
                               // ENet message ids occupy 0x00-0x1F. The non-ENet boundary was raised
                               // from 0x10 to 0x20 in #853 to free an ENet id for ConnectRequest -- a
                               // deliberate, documented choice (the prior comment flagged that breaking
                               // the "0x10+ = non-ENet" invariant should be a choice, not an accident).
};

// The role a peer joins as, chosen by the client in MsgConnectRequest and granted (possibly clamped) by
// the server in MsgConnectAck. An observer has no entity, no FlightIntegrator, and no controller (#857).
enum class PeerRole : uint8_t {
    Pilot = 0,    // flies an aircraft entity
    Observer = 1, // spectator/ghost camera; no entity spawned
};

// Validate an attacker-supplied role byte before casting to PeerRole (mirrors isWingmanCommandOrdinal).
inline bool isPeerRoleOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(PeerRole::Observer);
}

// Machine-readable reason carried in MsgConnectRefusal::code, alongside the human-readable text.
// Lets the client map a rejection to a localized string without parsing the English text.
enum class ConnectRefusalCode : uint8_t {
    Generic = 0,
    Banned = 1,
    AccessDenied = 2, // allowlist miss or admin-auth lockout
    RateLimited = 3,
    TooManyConnections = 4, // per-IP concurrent connection cap
    AdminLockout = 5,
    RoleDenied = 6,          // requested a role the server does not allow (e.g. observer when disabled) (#857)
    MissingRequiredPack = 7, // client lacks a server-required content pack (#872 refuse policy; the
                             // reason text carries the missing pack list)
    EntitlementRequired = 8, // premium content requires an entitlement token (RFC #871; reserved)
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

// Reliable, sent once on connect (after MsgHello) in reply to MsgConnectRequest, and again on a
// mid-session role change (#857). Followed by typeCount x MsgEntityTypeDef in the same packet.
//
// grantedRole (#853/#857) is the role the server GRANTED, which may differ from the requested one. It
// is load-bearing for the "rejected before ack" sentinel: an observer's valid ack carries
// assignedEntityGen == 0 (no entity), which is indistinguishable from a pre-ack rejection by idx alone,
// so the client keys its rejection detection on "did a ConnectAck arrive", not on idx == 0.
struct MsgConnectAck {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectAck)};
    uint8_t tickRateHz{60};
    uint16_t typeCount{0};
    uint32_t assignedEntityIdx{0}; // entity slot assigned to this peer (0 = none, e.g. observer)
    uint32_t assignedEntityGen{0}; // entity generation (0 = none assigned)
    float planetRadiusKm{0.f};     // planet sphere radius (km); Earth default = 6371
    uint8_t grantedRole{0};        // PeerRole granted by the server (Pilot/Observer) (#857)
    uint8_t reserved2[3]{};        // pad to 20 (multiple of alignof(MsgEntityTypeDef) so records stay aligned)
}; // 20 bytes, align 4 (multiple of alignof(MsgEntityTypeDef) so trailing records stay aligned)
static_assert(sizeof(MsgConnectAck) == 20u, "MsgConnectAck wire size changed");
static_assert(alignof(MsgConnectAck) == 4u, "MsgConnectAck alignment changed");
static_assert(sizeof(MsgConnectAck) % 4u == 0u, "MsgConnectAck not record-aligned (MsgEntityTypeDef is align 4)");
static_assert(offsetof(MsgConnectAck, typeCount) == 2u, "MsgConnectAck::typeCount offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityIdx) == 4u, "MsgConnectAck::assignedEntityIdx offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityGen) == 8u, "MsgConnectAck::assignedEntityGen offset changed");
static_assert(offsetof(MsgConnectAck, planetRadiusKm) == 12u, "MsgConnectAck::planetRadiusKm offset changed");
static_assert(offsetof(MsgConnectAck, grantedRole) == 16u, "MsgConnectAck::grantedRole offset changed");

// Entity type definition record appended after MsgConnectAck.
//
// flightModel (#811) is here because the client MUST integrate the same aircraft the server does.
// Without it, the client had no way to learn an entity type's flight model except to re-load the
// entity def from disk BY ITS ID -- a lookup for "entities/fl-base:f15c.toml", which cannot exist --
// and it silently fell back to the builtin UFO model. Permanent, invisible prediction divergence.
//
// payloadMassKg / payloadCd0 (#812) are the aggregate cost of the type's DEFAULT loadout. The client
// has no hardpoints and no WeaponRegistry, and must not need one: it needs two floats. When per-entity
// loadouts land (#583) these move into the snapshot record and this pair becomes the spawn default.
struct MsgEntityTypeDef {
    uint32_t typeIndex{0};
    char id[64]{};      // null-terminated type id, e.g. "builtin:debug-entity"
    char mesh[64]{};    // null-terminated mesh asset name; empty = builtin tetrahedron
    char dmgMesh[64]{}; // null-terminated damage mesh; empty = none
    // --- appended at the tail (#811); every offset above is unchanged, so this is additive ---
    char flightModel[64]{};   // ASSET NAME, not a def id; empty = builtin (UFO) flight model
    float payloadMassKg{0.f}; // default-loadout store mass (kg); 0 = clean airframe
    float payloadCd0{0.f};    // default-loadout parasite-drag delta; 0 = clean airframe
    // --- appended at the tail (#860); additive, prior offsets unchanged ---
    char name[64]{}; // friendly display name (EntityDef::name), e.g. "F-16C"; empty = fall back to id
    // --- appended at the tail (#886); additive, prior offsets unchanged ---
    uint8_t category{0};       // ObjectCategory ordinal; client gates via isObjectCategoryOrdinal before cast
    uint8_t projectileKind{0}; // ProjectileKind ordinal (Projectile types only); 0 = None
    uint8_t reservedCat[2]{};  // pad to 336 (multiple of align 4 so trailing records stay aligned)
}; // 336 bytes, align 4
static_assert(sizeof(MsgEntityTypeDef) == 336u, "MsgEntityTypeDef wire size changed");
static_assert(alignof(MsgEntityTypeDef) == 4u, "MsgEntityTypeDef alignment changed");
static_assert(sizeof(MsgEntityTypeDef) % alignof(MsgEntityTypeDef) == 0u, "MsgEntityTypeDef not record-aligned");
static_assert(offsetof(MsgEntityTypeDef, id) == 4u, "MsgEntityTypeDef::id offset changed");
static_assert(offsetof(MsgEntityTypeDef, mesh) == 68u, "MsgEntityTypeDef::mesh offset changed");
static_assert(offsetof(MsgEntityTypeDef, dmgMesh) == 132u, "MsgEntityTypeDef::dmgMesh offset changed");
static_assert(offsetof(MsgEntityTypeDef, flightModel) == 196u, "MsgEntityTypeDef::flightModel offset changed");
static_assert(offsetof(MsgEntityTypeDef, payloadMassKg) == 260u, "MsgEntityTypeDef::payloadMassKg offset changed");
static_assert(offsetof(MsgEntityTypeDef, payloadCd0) == 264u, "MsgEntityTypeDef::payloadCd0 offset changed");
static_assert(offsetof(MsgEntityTypeDef, name) == 268u, "MsgEntityTypeDef::name offset changed");
static_assert(offsetof(MsgEntityTypeDef, category) == 332u, "MsgEntityTypeDef::category offset changed");
static_assert(offsetof(MsgEntityTypeDef, projectileKind) == 333u, "MsgEntityTypeDef::projectileKind offset changed");

// Faction index -> id/name, sent once after ConnectAck for every registered faction (#860). The
// client maps a snapshot entity's factionIndex (carried on full records) to a display name for the
// observer entity picker, and later to friend/foe HUD colouring. Records are concatenated into one
// reliable packet whose leading msgId is FactionDef; the client reads size / sizeof(MsgFactionDef)
// of them, each self-describing via factionIndex, so order is irrelevant. Parsed via fl::readMsg.
struct MsgFactionDef {
    uint8_t msgId{static_cast<uint8_t>(MsgId::FactionDef)};
    uint8_t reserved{0};
    uint16_t factionIndex{0}; // FactionRegistry index this record describes
    char id[64]{};            // null-terminated faction id, e.g. "blue"
    char name[64]{};          // null-terminated display name, e.g. "Blue Coalition"; empty = fall back to id
}; // 132 bytes, align 2
static_assert(sizeof(MsgFactionDef) == 132u, "MsgFactionDef wire size changed");
static_assert(alignof(MsgFactionDef) == 2u, "MsgFactionDef alignment changed");
static_assert(offsetof(MsgFactionDef, factionIndex) == 2u, "MsgFactionDef::factionIndex offset changed");
static_assert(offsetof(MsgFactionDef, id) == 4u, "MsgFactionDef::id offset changed");
static_assert(offsetof(MsgFactionDef, name) == 68u, "MsgFactionDef::name offset changed");

// One mounted content pack, reported by the client in MsgConnectRequest's trailing manifest (#872 wire
// half). The server compares it against its required-pack set (Phase 4: warn-only). contentHash is
// reserved (zero-filled) until a pack-hashing pass lands, so adding hash enforcement is a no-wire-change
// step. Fixed-size null-terminated char fields; parsed via fl::readRecordAt (WireCodec.h).
struct PackManifestEntry {
    char id[64]{};             // null-terminated pack id, e.g. "fl-base"
    char version[32]{};        // null-terminated version string, e.g. "0.3.1"
    uint8_t contentHash[32]{}; // reserved: SHA-256-sized pack content hash; all-zero = not computed
}; // 128 bytes, align 1
static_assert(sizeof(PackManifestEntry) == 128u, "PackManifestEntry wire size changed");
static_assert(alignof(PackManifestEntry) == 1u, "PackManifestEntry alignment changed");
static_assert(offsetof(PackManifestEntry, version) == 64u, "PackManifestEntry::version offset changed");
static_assert(offsetof(PackManifestEntry, contentHash) == 96u, "PackManifestEntry::contentHash offset changed");

// Reliable, client->server: the FIRST packet the client sends on connect (#853). Replaces the old
// "client sends nothing; server unilaterally spawns" flow. The server replies MsgConnectAck (granting a
// role + assigning an entity) or MsgConnectRefusal. Followed in the same packet by packCount x
// PackManifestEntry records, then an optional TLV ext block (ExtTag 0x0500-0x05FF; reserved for the
// RFC #871 entitlement token). Unpacked/naturally aligned; parse via fl::readMsg + fl::readRecordAt.
struct MsgConnectRequest {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ConnectRequest)};
    uint8_t requestedRole{0}; // PeerRole the client asks to join as (server may clamp/deny)
    uint16_t protocolVersion{kProtocolVersion};
    uint16_t packCount{0};          // number of trailing PackManifestEntry records
    uint16_t reserved{0};           // pad to 8; future join flags
    char requestedEntityType[64]{}; // null-terminated type id to fly; empty = server default (#834)
}; // 72 bytes, align 2 (multiple of alignof(PackManifestEntry)=1 so trailing records stay aligned)
static_assert(sizeof(MsgConnectRequest) == 72u, "MsgConnectRequest wire size changed");
static_assert(alignof(MsgConnectRequest) == 2u, "MsgConnectRequest alignment changed");
static_assert(offsetof(MsgConnectRequest, protocolVersion) == 2u, "MsgConnectRequest::protocolVersion offset changed");
static_assert(offsetof(MsgConnectRequest, packCount) == 4u, "MsgConnectRequest::packCount offset changed");
static_assert(offsetof(MsgConnectRequest, requestedEntityType) == 8u,
              "MsgConnectRequest::requestedEntityType offset changed");

// Unreliable, unicast per-peer every sim tick.
// Body layout after this 24-byte header (#725 shared-origin encode-once):
//   1. ORIGIN TABLE: originCount entries of double[3] (8-aligned; each a shared grid-cell quantization
//      origin, SnapshotCodec::originForPos), at offset sizeof(MsgWorldSnapshotHeader).
//   2. STITCHED RECORD STREAM: recordCount byte-aligned entity records occupying bitstreamBytes bytes,
//      starting at sizeof(MsgWorldSnapshotHeader) + originCount * sizeof(double[3]). Each record is
//      prefixed with an origin-index varint into the origin table; positions are quantized RELATIVE to
//      that origin, so a record is peer-INDEPENDENT and the sim encodes it once per tick (see
//      SnapshotCodec.h).
//   3. TLV extension block, immediately after the record stream.
// Sized to 24 (multiple of 8) so the origin table's doubles stay 8-aligned and the fixed header can be
// read in place via fl::viewMsg / fl::readMsg.
// MsgWorldSnapshotHeader::flags bits (#775). When kSnapshotFlagCompressed is set, everything after
// this 24-byte header (origin table + record stream + TLV block) is one zstd frame;
// `uncompressedBytes` is the exact decompressed byte length (bounded by the receiver before
// allocating). recordCount/bitstreamBytes/originCount always describe the UNCOMPRESSED layout.
inline constexpr uint16_t kSnapshotFlagCompressed = 0x0001u;

struct MsgWorldSnapshotHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WorldSnapshot)};       // @0 (byte-0 dispatch unchanged)
    uint8_t protocolVersion{static_cast<uint8_t>(kProtocolVersion)}; // @1
    uint16_t recordCount{0};       // @2 number of stitched entity records in the record stream
    uint32_t bitstreamBytes{0};    // @4 byte length of the record stream (after the origin table; TLV follows)
    uint64_t tickIndex{0};         // @8
    uint16_t originCount{0};       // @16 number of double[3] origins in the table between this header and the stream
    uint16_t flags{0};             // @18 kSnapshotFlag* bits (#775); 0 = raw payload
    uint32_t uncompressedBytes{0}; // @20 decompressed payload length when kSnapshotFlagCompressed; else 0
}; // 24 bytes, align 8
static_assert(sizeof(MsgWorldSnapshotHeader) == 24u, "MsgWorldSnapshotHeader wire size changed");
static_assert(alignof(MsgWorldSnapshotHeader) == 8u, "MsgWorldSnapshotHeader alignment changed");
static_assert(offsetof(MsgWorldSnapshotHeader, recordCount) == 2u,
              "MsgWorldSnapshotHeader::recordCount offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, bitstreamBytes) == 4u,
              "MsgWorldSnapshotHeader::bitstreamBytes offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, tickIndex) == 8u, "MsgWorldSnapshotHeader::tickIndex offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, originCount) == 16u,
              "MsgWorldSnapshotHeader::originCount offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, flags) == 18u, "MsgWorldSnapshotHeader::flags offset changed");
static_assert(offsetof(MsgWorldSnapshotHeader, uncompressedBytes) == 20u,
              "MsgWorldSnapshotHeader::uncompressedBytes offset changed");

// Unreliable, client->server, sent each render frame. Padded to 48 (multiple of 8 for tickIndex).
struct MsgClientInput {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ClientInput)};
    uint8_t buttons{0}; // bit 0 = gun trigger (level), bit 1 = afterburner, bit 2 = fire selected store
    uint16_t protocolVersion{kProtocolVersion};
    uint32_t seqNum{0};    // monotonically increasing; server discards packets not newer than last accepted
    uint64_t tickIndex{0}; // server's tickIndex from last received WorldSnapshot; server uses delta for delay estimate
    float throttle{0.f};   // [0.0, 1.0]
    float elevator{0.f};   // [-1.0, +1.0] nose-up positive
    float aileron{0.f};    // [-1.0, +1.0] right-roll positive
    float rudder{0.f};     // [-1.0, +1.0] right-yaw positive
    float viewAxis[3]{};   // normalized look direction (world space)
    uint32_t ackMask{0};   // @44 selective-ack bitmask (#566): bit b = decoded snapshot tick tickIndex-1-b

    // Weapon-station selection (#625). ABSOLUTE, not cycle-edges: the client computes cycling
    // locally and sends the result, so selection converges on an unreliable channel where an edge
    // can be lost. 255 = no station. Server clamps to the entity's station count.
    uint8_t selectedStation{255};
    uint8_t reservedB[3]{}; // explicit padding — the layout rule forbids implicit holes
    uint32_t reservedC{0};  // pads to the struct's 8-byte alignment; future fire fields land here

    // Camera eye world-position (#858). The client sends where it is LOOKING FROM each frame so the
    // server can center interest management on an entity-less peer (an observer ghost camera, or a
    // dead peer awaiting respawn) — a peer with no aircraft has no transform to key interest on.
    // Ignored for a pilot (its aircraft transform wins). Double, absolute world metres — matches the
    // engine's double world positions; ~200 km interest radius makes sub-metre precision irrelevant,
    // but keeping it double avoids a quantization origin the client does not know. @56, 8-aligned.
    double cameraEye[3]{};
}; // 80 bytes, align 8
static_assert(sizeof(MsgClientInput) == 80u, "MsgClientInput wire size changed");
static_assert(alignof(MsgClientInput) == 8u, "MsgClientInput alignment changed");
static_assert(offsetof(MsgClientInput, seqNum) == 4u, "MsgClientInput::seqNum offset changed");
static_assert(offsetof(MsgClientInput, tickIndex) == 8u, "MsgClientInput::tickIndex offset changed");
static_assert(offsetof(MsgClientInput, throttle) == 16u, "MsgClientInput::throttle offset changed");
static_assert(offsetof(MsgClientInput, viewAxis) == 32u, "MsgClientInput::viewAxis offset changed");
static_assert(offsetof(MsgClientInput, ackMask) == 44u, "MsgClientInput::ackMask offset changed");
static_assert(offsetof(MsgClientInput, selectedStation) == 48u, "MsgClientInput::selectedStation offset changed");
static_assert(offsetof(MsgClientInput, cameraEye) == 56u, "MsgClientInput::cameraEye offset changed");

// Unreliable, server->client, broadcast every 10 sim ticks (~6 Hz at 60 Hz).
// timeOfDayTenths: encode timeOfDay as uint16 (hours * 10) to keep it 2-aligned.
struct MsgWeatherState {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WeatherState)};
    uint8_t preset{0};           // WeatherPreset cast to uint8_t
    uint16_t timeOfDayTenths{0}; // hours * 10; decode: / 10.f; range [0, 239]
    float fogDensity{0.f};
    float fogStartDist{5000.f};
    float windX{0.f};         // world-frame wind x (m/s), includes gust component
    float windZ{0.f};         // world-frame wind z (m/s), includes gust component
    float turbulenceAmp{0.f}; // #426: turbulence amplitude (m/s). The client feeds it to the SAME
                              // deterministic weatherTurbulence(entityIdx, tickIndex, amp) the server
                              // uses, so prediction reproduces per-tick turbulence exactly instead of
                              // predicting zero and jittering. Tail-append, additive; no version bump.
}; // 24 bytes, align 4
static_assert(sizeof(MsgWeatherState) == 24u, "MsgWeatherState wire size changed");
static_assert(offsetof(MsgWeatherState, turbulenceAmp) == 20u, "MsgWeatherState::turbulenceAmp offset changed");
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
    uint8_t reserved[3]{}; // pad so ackMask is 4-aligned
    uint32_t ackMask{0};   // @4 selective-ack bitmask (#566); same semantic as MsgClientInput::ackMask
    uint64_t tickIndex{0}; // last received WorldSnapshot tickIndex (same semantic as MsgClientInput)
}; // 16 bytes, align 8
static_assert(sizeof(MsgHeartbeat) == 16u, "MsgHeartbeat wire size changed");
static_assert(alignof(MsgHeartbeat) == 8u, "MsgHeartbeat alignment changed");
static_assert(offsetof(MsgHeartbeat, ackMask) == 4u, "MsgHeartbeat::ackMask offset changed");
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

// ---------------------------------------------------------------------------------------------
// Combat event channel (#626)
// ---------------------------------------------------------------------------------------------
// The reliable gameplay-event stream: kill credit and per-peer combat stats. Cosmetic effects
// (tracers, impacts, detonations) deliberately do NOT ride here — they are unreliable-by-design
// snapshot TLVs (#625), because a lost muzzle flash is nothing and a lost kill credit is a bug.

enum class CombatEventType : uint8_t {
    Kill = 0,  // broadcast: subject was destroyed; instigator gets the credit
    Stats = 1, // unicast: the receiving peer's own running tallies (kills/losses/score)
};

// Cosmetic weapon effects (#625), carried as SnapshotEffects TLV records on the UNRELIABLE
// snapshot — a lost muzzle flash is nothing; anything that must arrive uses MsgCombatEvent.
// Unknown values must be skipped by the client, never rejected.
enum class EffectType : uint8_t {
    WeaponFired = 0,   // gunfire: tracer/muzzle at pos, srcIdx = shooter
    MissileLaunch = 1, // store left the rails: srcIdx = shooter
    Impact = 2,        // a round connected: pos = hit, tgtIdx = struck entity
    Detonation = 3,    // warhead burst at pos
    NuclearFlash = 4,  // full-screen flash cue; pos = ground zero
};

// One packed SnapshotEffects TLV record. Written byte-serially (memcpy per field) into the TLV
// payload — the TLV data area is unaligned by design.
inline constexpr std::size_t kEffectRecordBytes = 22; // type u8 + weaponClass u8 + src u32 + tgt u32 + pos f32[3]
inline constexpr std::size_t kMaxEffectsPerSnapshot = 16;

struct MsgCombatEventHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::CombatEvent)};
    uint8_t count{0}; // CombatEventRecord entries following this header
    uint16_t reserved{0};
}; // 4 bytes, align 2
static_assert(sizeof(MsgCombatEventHeader) == 4u, "MsgCombatEventHeader wire size changed");
static_assert(offsetof(MsgCombatEventHeader, count) == 1u, "MsgCombatEventHeader::count offset changed");

// One multiplexed combat event. Field meaning depends on `type`:
//   Kill:  subject = the destroyed entity, instigator = the credited entity (null idx = environment);
//          a = instigator's owning peer id (kNoOwningPeer = AI/server), b = subject's owning peer id.
//   Stats: a = kills, b = losses, c = score — the RECEIVING peer's own tallies (unicast only).
inline constexpr uint32_t kNoOwningPeer = 0xFFFFFFFFu; // peer id 0 is a real player (#610's kNoPeer rule)

struct CombatEventRecord {
    uint8_t type{0};        // CombatEventType
    uint8_t weaponClass{0}; // WeaponType ordinal of the credited weapon; 0xFF = none/unknown
    uint16_t reserved{0};
    uint32_t subjectIdx{0};
    uint16_t subjectGen{0};
    uint16_t pad0{0};
    uint32_t instigatorIdx{0};
    uint16_t instigatorGen{0};
    uint16_t pad1{0};
    uint32_t a{0};
    uint32_t b{0};
    int32_t c{0};
}; // 32 bytes, align 4
static_assert(sizeof(CombatEventRecord) == 32u, "CombatEventRecord wire size changed");
static_assert(alignof(CombatEventRecord) == 4u, "CombatEventRecord alignment changed");
static_assert(offsetof(CombatEventRecord, subjectIdx) == 4u, "CombatEventRecord::subjectIdx offset changed");
static_assert(offsetof(CombatEventRecord, instigatorIdx) == 12u, "CombatEventRecord::instigatorIdx offset changed");
static_assert(offsetof(CombatEventRecord, a) == 20u, "CombatEventRecord::a offset changed");
static_assert(offsetof(CombatEventRecord, c) == 28u, "CombatEventRecord::c offset changed");

// ---------------------------------------------------------------------------------------------
// Flight command channel (#610)
// ---------------------------------------------------------------------------------------------
// The scripted wingman order path. The unit of organisation is the FLIGHT: a lead plus N members,
// where N may be 1 or 6+. "Wingman" is the radio VOCABULARY (engine/ai/WingmanCommand.h), not the
// data model -- so these messages address flight MEMBERS by entity index, and nothing here assumes a
// member is AI or that a flight has exactly one of them.
//
// A member may be an AI aircraft OR ANOTHER PLAYER. Those are handled differently and the difference
// is visible on the wire:
//   * AI member    -> the server retasks its controller. The commander gets Acknowledged.
//   * HUMAN member -> the server CANNOT retask a person. The order is RELAYED to that player's client
//                     as a radio call (WingmanResult::Relayed, delivered TO the member), and the
//                     commander is told it was passed on. Compliance is the human's choice. A flight
//                     of two players is therefore a comms structure, not a control structure -- which
//                     is the only honest model of it.
//
// THE COMMANDER NEED NOT BE IN THE FLIGHT. A flight is {id, anchor entity, commander, members}, and
// the commander is a ROLE, not "the aircraft in front":
//   * A player leading their own flight is the common case: commander == the peer flying the anchor.
//   * An ALL-AI flight is commanded from outside it -- by an AWACS/GCI player, or by a game master
//     through the `flight` admin command family. Neither is a member; both order it the same way.
// That is why orders address a FLIGHT ID rather than "my wingman", and why authority is checked as
// "are you this flight's commander", not "do you own this entity".
//
// Deliberately NOT MsgAdminCommand: that is a password-gated shell tunnel, and a gameplay order is
// authorized by COMMANDING THE FLIGHT, not by a token. The server resolves the flight from the id
// and rejects any packet whose sender is not its commander -- authority never comes out of the packet.
//
// The command ordinal is fl::ai::WingmanCommand (engine/ai/WingmanCommand.h), which this header
// cannot include: engine-protocol must reach only the stdlib (cmake/layering.cmake enforces it).
// kWingmanCommandCount below mirrors that enum's size, and fl-server static_asserts the two agree in
// a TU that sees both, so they cannot drift silently.

// Number of commands in the scripted grammar; mirrors fl::ai::WingmanCommand::Count.
static constexpr uint8_t kWingmanCommandCount = 6;

// MsgWingmanCommand::memberIdx sentinel: address the whole flight rather than one member.
static constexpr uint32_t kFlightAll = 0xFFFFFFFFu;
// MsgWingmanAck::targetIdx sentinel: no target was designated (every command except attack_my_target,
// and attack_my_target itself when nothing lay in the boresight cone).
static constexpr uint32_t kNoTarget = 0xFFFFFFFFu;

// Outcome of a flight order. A machine-readable code, NOT server-authored text: brevity calls are UI
// strings that must be localizable (same reasoning as ConnectRefusalCode), the client already needs
// this table to label its radio menu, and it keeps a server-controlled string off the HUD text path.
enum class WingmanResult : uint8_t {
    Acknowledged = 0, // order accepted; an AI member's behavior changed
    NoFlight = 1,     // sender leads no flight with live members, OR named a member that is not in it --
                      // deliberately the SAME code for both, so a peer cannot probe which entity
                      // indices exist or who they belong to
    NoTarget = 2,     // attack_my_target: nothing hostile in the lead's boresight cone; behavior unchanged
    Unavailable = 3,  // the addressed member is dead
    Rejected = 4,     // unknown command ordinal
    RateLimited = 5,  // too many orders in the rate-limit window (acked once per window, never per packet)
    CheckIn = 6,      // unsolicited, sent TO a lead once on connect: your flight is formed, this is its
                      // size. MsgConnectAck cannot carry this -- it is immediately followed by
                      // MsgEntityTypeDef records, so appending a field there would shift them
                      // (breaking, not additive).
    Relayed = 7,      // sent TO A HUMAN MEMBER: your lead has ordered `command`. Advisory -- the server
                      // does not and cannot make a player comply. Also returned to the LEAD when every
                      // addressed member was human, so the lead knows the call went out rather than
                      // believing an aircraft was retasked.
    NotLead = 8,      // sender is in a flight but is not its lead; only the lead issues orders
};

// MsgWingmanCommand::flightId sentinel: "the formation I command", resolved server-side. The radio
// menu of a pilot who leads exactly one flight never has to know its id; an AWACS or package
// commander who commands several MUST name one, because "my flight" is then ambiguous.
static constexpr uint16_t kOwnFlight = 0xFFFFu;

// "No formation." Mirrors fl::kNoFormation (engine/world/Formation.h), which this header cannot
// include -- engine-protocol must reach only the stdlib. WorldBroadcaster static_asserts they agree.
static constexpr uint16_t kNoFlightId = 0u;

// MsgWingmanCommand::flags
// Cascade the order to every sub-formation beneath the addressed one — a package commander telling
// the whole package to RTB, rather than each flight in turn. Without it the order stops at the
// addressed node's own members.
static constexpr uint8_t kFlightFlagCascade = 0x01u;

// Order a formation (or one member of it). Client->server, RELIABLE (an order must not be dropped).
struct MsgWingmanCommand {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WingmanCommand)};
    uint8_t command{0}; // fl::ai::WingmanCommand ordinal; >= kWingmanCommandCount is rejected
    uint16_t protocolVersion{kProtocolVersion};
    uint32_t memberIdx{kFlightAll}; // entity pool index of the addressed member, or kFlightAll
    uint32_t seqNum{0};             // client-monotonic; server discards packets not newer (dup/reorder guard)
    uint16_t flightId{kOwnFlight};  // formation to order; kOwnFlight = the one this peer commands
    uint8_t flags{0};               // kFlightFlagCascade
    uint8_t reserved{0};
}; // 16 bytes, align 4
static_assert(sizeof(MsgWingmanCommand) == 16u, "MsgWingmanCommand wire size changed");
static_assert(alignof(MsgWingmanCommand) == 4u, "MsgWingmanCommand alignment changed");
static_assert(offsetof(MsgWingmanCommand, command) == 1u, "MsgWingmanCommand::command offset changed");
static_assert(offsetof(MsgWingmanCommand, protocolVersion) == 2u, "MsgWingmanCommand::protocolVersion offset changed");
static_assert(offsetof(MsgWingmanCommand, memberIdx) == 4u, "MsgWingmanCommand::memberIdx offset changed");
static_assert(offsetof(MsgWingmanCommand, seqNum) == 8u, "MsgWingmanCommand::seqNum offset changed");
static_assert(offsetof(MsgWingmanCommand, flightId) == 12u, "MsgWingmanCommand::flightId offset changed");
static_assert(offsetof(MsgWingmanCommand, flags) == 14u, "MsgWingmanCommand::flags offset changed");

// Outcome of an order (to the commander), the on-connect check-in (to a commander), or a relayed
// radio call (to a human member). Server->client, reliable.
struct MsgWingmanAck {
    uint8_t msgId{static_cast<uint8_t>(MsgId::WingmanAck)};
    uint8_t command{0};             // echoed WingmanCommand ordinal (Rejoin for a check-in)
    uint8_t result{0};              // WingmanResult
    uint8_t flightSize{0};          // live members in the addressed formation right now (0 = none)
    uint32_t memberIdx{kFlightAll}; // member the order applied to, or kFlightAll. On a Relayed call to
                                    // a human member this is the CALLER's entity index — who is
                                    // ordering them.
    uint32_t targetIdx{kNoTarget};  // designated target for attack_my_target, else kNoTarget
    uint16_t flightId{kNoFlightId}; // formation this ack refers to; the check-in is how a client
                                    // learns the id of the flight it commands
    uint16_t reserved{0};
}; // 16 bytes, align 4
static_assert(sizeof(MsgWingmanAck) == 16u, "MsgWingmanAck wire size changed");
static_assert(alignof(MsgWingmanAck) == 4u, "MsgWingmanAck alignment changed");
static_assert(offsetof(MsgWingmanAck, result) == 2u, "MsgWingmanAck::result offset changed");
static_assert(offsetof(MsgWingmanAck, flightSize) == 3u, "MsgWingmanAck::flightSize offset changed");
static_assert(offsetof(MsgWingmanAck, memberIdx) == 4u, "MsgWingmanAck::memberIdx offset changed");
static_assert(offsetof(MsgWingmanAck, targetIdx) == 8u, "MsgWingmanAck::targetIdx offset changed");
static_assert(offsetof(MsgWingmanAck, flightId) == 12u, "MsgWingmanAck::flightId offset changed");

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
//   0x0100–0x01FF  MsgWorldSnapshot extensions (appended after the quantized record bitstream)
//   0x0200–0x02FF  MsgConnectAck extensions (reserved for future use)
//   0x0300–0x03FF  MsgClientInput extensions (reserved for future use)
//   0x0400–0x04FF  MsgWeatherState extensions (reserved for future use)
//   0x0500–0x05FF  MsgConnectRequest extensions (reserved: RFC #871 entitlement token block)
//   Values outside defined ranges are reserved and must not be sent.
enum class ExtTag : uint16_t {
    SnapshotPeerCount = 0x0100,   // uint16_t: active connected peer count at snapshot time
    SnapshotPeerLatency = 0x0101, // uint16_t: receiving peer's one-way latency in ms (estimatedDelayTicks*1000/60);
                                  // absent when delay is 0
    SnapshotPeerDelayTicks =
        0x0102,               // uint16_t: raw estimatedDelayTicks for client-side prediction replay depth;
                              // avoids ms rounding loss; absent when delay is 0; emitted alongside SnapshotPeerLatency
    SnapshotDespawn = 0x0103, // uint32_t[]: indices of entities the receiving peer KNEW that were removed from the sim
                              // (kills/despawns, not interest-out); priority/budget scheduler (#516). Variable length =
                              // 4*count; little-endian; read per-element via memcpy (payload is unaligned). Empty =
                              // omitted. Repeated for a few ticks on the unreliable channel for drop tolerance.
    SnapshotEffects = 0x0104, // EffectType records (#625), kEffectRecordBytes each, little-endian, unaligned:
                              // cosmetic weapon effects within the receiving peer's interest radius, capped at
                              // kMaxEffectsPerSnapshot. Unreliable by design — a dropped packet loses cosmetics,
                              // never state. pos is float32 world position (particles do not need 0.125 m).
    SnapshotLastAckedSeqNum =
        0x0105, // uint32_t: seqNum of the last MsgClientInput the server drained + APPLIED for the receiving
                // peer (#427). Lets client prediction replay EXACTLY the inputs the server has not yet
                // reflected (history seqNum > this), instead of approximating the replay depth from
                // estimatedDelayTicks. Omitted until the first input is applied (a peer's first snapshots).
};

} // namespace fl
