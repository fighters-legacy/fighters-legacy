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
    Datalink = 0x12,           // server->client, unreliable: the peer's fused team track picture + RWR
                               // (#528), sent per-peer at ~6 Hz. Loss-tolerant (refreshed every send);
                               // carries what THIS peer's team can see, positions relative to a header
                               // origin. See MsgDatalinkHeader / DatalinkTrack / DatalinkThreat below.
    CrewRoster = 0x13,         // server->client, reliable: one crewed aircraft's full seat roster (#972).
                               // Sent after MsgConnectAck for the peer's own crewed aircraft, and on any
                               // seat occupancy change (#974). Single-seat aircraft never send one (the
                               // implicit-single-pilot fast path). See MsgCrewRosterHeader / CrewRosterSeat.
    SeatRequest = 0x14,        // client->server, reliable: claim a non-fly crew seat, or leave the current
                               // seat (#974). {entityId, seatIndex} or the leave flag. The server replies
                               // MsgSeatResult and, on a grant, re-sends MsgConnectAck + the roster delta.
    SeatResult = 0x15,         // server->client, reliable: outcome of a MsgSeatRequest (SeatResultCode).
    MusicState = 0x16,         // server->client, reliable: request a client music-state transition (#413/
                               // #166). Broadcast when a Lua/mission script calls world.set_music_state();
                               // carries a GameState ordinal. Additive id, old clients discard.
    Haptic = 0x17,             // server->client, reliable: a scripted rumble/trigger-rumble/stop request
                               // (#128) from a Lua script's rumble()/rumble_triggers()/stop_rumble().
                               // Each client plays it on its local (current-player) gamepad. Additive id.
    MissionOutcome = 0x18,     // server->client, reliable: the objective evaluator ended the mission
                               // (#584). Carries success/failure + elapsed/triggers so the debrief shows
                               // the real outcome instead of a hardcoded success. Additive id.
    RadioCommand = 0x19,       // client->server, reliable: a player radio command (#703). Verb-routed
                               // ("atc request_takeoff|request_landing|inbound|cancel [facility]"); the
                               // `wing` verb namespace is reserved for #610. The server dispatches to the
                               // ATC service and replies with MsgRadioTransmission(s). NOTE: 0x0D/0x0E
                               // (the id #703 originally reserved) were taken by the wingman channel, so
                               // the radio channel took the next free ENet ids 0x19/0x1A.
    RadioTransmission = 0x1A,  // server->client, reliable: one spoken radio line (#703) — speaker,
                               // localizable text, a stable voiceKey (TTS/pack OGG), and a subtitle
                               // dwell. Unicast (directed) or broadcast. Additive id, old clients discard.
    MissionRoster = 0x1B,      // server->client, reliable: entity index/gen -> mission object id table
                               // (#914). Sent after ConnectAck (beside FactionDef) with the current
                               // spawned mission objects + bound player slots, so the cinematic recorder
                               // (#909) can resolve entity-relative camera shots ("orbit bandit1") to
                               // network entities. Self-describing concatenated records; additive id, old
                               // clients discard. See MsgMissionRoster below.
    // --- Epic E multiplayer gameplay framework (#497) ---
    PlayerRoster = 0x1C, // server->client, reliable: participant id -> callsign/faction/role table (#996).
                         // Upsert/leave stream; the single name source for chat, kill feed and scoreboard.
    MatchState = 0x1D,   // server->client, reliable: match phase + per-team scores + limits (#523).
    Scoreboard = 0x1E,   // server->client, unreliable: per-participant kills/deaths/score/ping (#523).
    TeamRequest = 0x1F,  // client->server, reliable: request a mid-match team/faction switch (#522).
    Chat = 0x20,         // client->server, reliable: a player chat line (channel + UTF-8 text) (#646).
    ChatEvent = 0x21,    // server->client, reliable: a routed chat line (sender + channel + text) (#646).
    // ENet message ids occupy 0x00-0x3F. The non-ENet (raw-UDP) boundary was raised from 0x20 to 0x40 in
    // #996 to make room for the Epic E ENet messages above (it was raised from 0x10 to 0x20 in #853). A
    // raw-UDP id lives at 0x40+ and is NEVER dispatched through the ENet onReceive path.
    LanBeacon = 0x40,   // raw UDP broadcast - NOT sent over ENet; 0x40+ reserved for non-ENet ids.
    ServerQuery = 0x41, // client->server raw UDP on the query port: A2S-style info request (#997).
    ServerInfo = 0x42,  // server->client raw UDP: reply to MsgServerQuery, carries live details (#997).
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
    MatchFull = 9,           // every team in the current game mode is at capacity (#522)
    BadPassword = 10,        // the server requires a join password and the client's was missing/wrong (#998)
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
    uint8_t reserved2[3]{};        // pad so peerId stays 4-aligned
    // --- appended at the tail (#996); additive, prior offsets unchanged ---
    // The peer's own transport peer id. The client had no way to learn its own id, which the roster
    // "you" highlight (#996) and chat self-echo (#646) need. Stable for the life of the connection.
    uint32_t peerId{0}; // @20 this peer's own id (matches PlayerRosterEntry::participantId)
}; // 24 bytes, align 4 (multiple of alignof(MsgEntityTypeDef) so trailing records stay aligned)
static_assert(sizeof(MsgConnectAck) == 24u, "MsgConnectAck wire size changed");
static_assert(alignof(MsgConnectAck) == 4u, "MsgConnectAck alignment changed");
static_assert(sizeof(MsgConnectAck) % 4u == 0u, "MsgConnectAck not record-aligned (MsgEntityTypeDef is align 4)");
static_assert(offsetof(MsgConnectAck, typeCount) == 2u, "MsgConnectAck::typeCount offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityIdx) == 4u, "MsgConnectAck::assignedEntityIdx offset changed");
static_assert(offsetof(MsgConnectAck, assignedEntityGen) == 8u, "MsgConnectAck::assignedEntityGen offset changed");
static_assert(offsetof(MsgConnectAck, planetRadiusKm) == 12u, "MsgConnectAck::planetRadiusKm offset changed");
static_assert(offsetof(MsgConnectAck, grantedRole) == 16u, "MsgConnectAck::grantedRole offset changed");
static_assert(offsetof(MsgConnectAck, peerId) == 20u, "MsgConnectAck::peerId offset changed");

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
    uint8_t reservedCat[2]{};  // pad (kept so the #886 offsets stay frozen)
    // --- appended at the tail (#38); additive, prior offsets unchanged ---
    // Flight-deck footprint for a type that accepts landings (EntityDef::deck). 0 = no deck. The
    // client composes its prediction ground floor as max(terrain, moving deck) from these, exactly
    // as the server does — without them a carrier landing predicts through the deck and snaps.
    // Catapult/arrest parameters deliberately do NOT travel: those events are server-authoritative.
    float deckLengthM{0.f};
    float deckWidthM{0.f};
    float deckHeightM{0.f};
}; // 348 bytes, align 4
static_assert(sizeof(MsgEntityTypeDef) == 348u, "MsgEntityTypeDef wire size changed");
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
static_assert(offsetof(MsgEntityTypeDef, deckLengthM) == 336u, "MsgEntityTypeDef::deckLengthM offset changed");
static_assert(offsetof(MsgEntityTypeDef, deckHeightM) == 344u, "MsgEntityTypeDef::deckHeightM offset changed");

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

// One entity <-> mission-object-id mapping (#914). Self-describing concatenated records (no header),
// exactly like MsgFactionDef: the client reads size / sizeof(MsgMissionRoster) of them. Sent reliably
// after ConnectAck with the current spawned mission objects + bound player slots, so the cinematic
// recorder's ShotDirector can resolve an entity-relative shot's target/look_at (a mission object id)
// to a live network entity. entityGen is carried so a pool-slot reuse never resolves to an impostor.
struct MsgMissionRoster {
    uint8_t msgId{static_cast<uint8_t>(MsgId::MissionRoster)};
    uint8_t reserved{0};
    uint16_t entityGen{0}; // EntityId generation of the mapped entity
    uint32_t entityIdx{0}; // EntityId pool index of the mapped entity
    char objectId[64]{};   // null-terminated mission object id, e.g. "bandit1"
}; // 72 bytes, align 4
static_assert(sizeof(MsgMissionRoster) == 72u, "MsgMissionRoster wire size changed");
static_assert(alignof(MsgMissionRoster) == 4u, "MsgMissionRoster alignment changed");
static_assert(offsetof(MsgMissionRoster, entityGen) == 2u, "MsgMissionRoster::entityGen offset changed");
static_assert(offsetof(MsgMissionRoster, entityIdx) == 4u, "MsgMissionRoster::entityIdx offset changed");
static_assert(offsetof(MsgMissionRoster, objectId) == 8u, "MsgMissionRoster::objectId offset changed");

// Runtime occupancy of a crew seat (#972). Authoring is two-state (Bot|Empty, SeatOccupancyDefault);
// a human claiming a seat (#974) is a RUNTIME state, so the WIRE occupancy is three-state. Validate an
// attacker-supplied byte with isSeatOccupancyOrdinal before casting.
enum class SeatOccupancy : uint8_t {
    Empty = 0, // nobody in the seat (a Fire/turret channel it owns goes silent)
    Bot = 1,   // an AI seat controller drives it (the authored default)
    Human = 2, // a peer occupies it (occupantPeerId names the peer)
};

inline bool isSeatOccupancyOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(SeatOccupancy::Human);
}

// One seat in a crewed aircraft's roster (#972). Concatenated after MsgCrewRosterHeader. Reliable, so
// the client always has a consistent occupancy picture for the seat-selection UI (#975) and to label a
// gunner station (#979). Capabilities are the machine contract (CrewCapabilityMask); role is the
// display string (#944 roles-as-data). Parsed via fl::readRecordAt.
struct CrewRosterSeat {
    uint8_t seatIndex{0};                 // @0 seat ordinal within the aircraft
    uint8_t occupancy{0};                 // @1 SeatOccupancy ordinal
    uint16_t capabilities{0};             // @2 CrewCapabilityMask (Fly/Fire/Radar/Countermeasures/Command)
    uint32_t occupantPeerId{0xFFFFFFFFu}; // @4 human peer id when occupancy==Human, else kNoSeatPeer sentinel
    uint8_t skillPct{50};                 // @8 round(per-instance skill * 100), [0,100]
    uint8_t turretIndex{255};             // @9 turret this seat aims (index into the aircraft's turrets); 255 = none
    uint8_t knockedOut{0};                // @10 #978: 1 = the seat is knocked out (silent); orthogonal to occupancy
    uint8_t reserved{0};                  // @11 pad to 12 (role stays 4-aligned)
    char role[32]{};                      // @12 null-terminated display string, e.g. "pilot" / "tail-gunner"
}; // 44 bytes, align 4
static_assert(sizeof(CrewRosterSeat) == 44u, "CrewRosterSeat wire size changed");
static_assert(alignof(CrewRosterSeat) == 4u, "CrewRosterSeat alignment changed");
static_assert(offsetof(CrewRosterSeat, capabilities) == 2u, "CrewRosterSeat::capabilities offset changed");
static_assert(offsetof(CrewRosterSeat, occupantPeerId) == 4u, "CrewRosterSeat::occupantPeerId offset changed");
static_assert(offsetof(CrewRosterSeat, skillPct) == 8u, "CrewRosterSeat::skillPct offset changed");
static_assert(offsetof(CrewRosterSeat, role) == 12u, "CrewRosterSeat::role offset changed");

// Header of a crewed aircraft's seat roster (#972). Followed by seatCount CrewRosterSeat records. The
// client keys the roster by (entityIdx, entityGen) so a pool-slot reuse never applies a stale roster.
// turretCount lets the client size its turret-orientation arrays before the SnapshotCrew TLV arrives.
struct MsgCrewRosterHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::CrewRoster)}; // @0
    uint8_t seatCount{0};                                   // @1 number of trailing CrewRosterSeat records
    uint8_t turretCount{0};                                 // @2 number of turret mounts on the aircraft
    uint8_t reserved{0};                                    // @3 pad
    uint32_t entityIdx{0};                                  // @4 aircraft entity index
    uint32_t entityGen{0};                                  // @8 aircraft entity generation
}; // 12 bytes, align 4 (multiple of alignof(CrewRosterSeat) so trailing records stay aligned)
static_assert(sizeof(MsgCrewRosterHeader) == 12u, "MsgCrewRosterHeader wire size changed");
static_assert(alignof(MsgCrewRosterHeader) == 4u, "MsgCrewRosterHeader alignment changed");
static_assert(sizeof(MsgCrewRosterHeader) % alignof(CrewRosterSeat) == 0u, "MsgCrewRosterHeader not record-aligned");
static_assert(offsetof(MsgCrewRosterHeader, entityIdx) == 4u, "MsgCrewRosterHeader::entityIdx offset changed");
static_assert(offsetof(MsgCrewRosterHeader, entityGen) == 8u, "MsgCrewRosterHeader::entityGen offset changed");

// Outcome of a MsgSeatRequest (#974). Granted or a specific denial reason. Validate an untrusted byte
// with isSeatResultOrdinal before casting.
enum class SeatResultCode : uint8_t {
    Granted = 0,             // the seat is now this peer's (join) / vacated (leave)
    NoSuchEntity = 1,        // the named entity does not exist / generation mismatch
    NotCrewed = 2,           // the entity is single-seat (no joinable seats)
    NoSuchSeat = 3,          // seatIndex out of range
    SeatOccupiedByHuman = 4, // another human already holds the seat (humans never displace humans)
    FlySeatNotJoinable = 5,  // the Fly seat belongs to the aircraft's owning pilot; spawn your own to fly
    NotInSeat = 6,           // a leave request from a peer that holds no seat
};

inline bool isSeatResultOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(SeatResultCode::NotInSeat);
}

// Client->server request to claim a non-fly crew seat (join) or leave the current seat. `leave` (bit 0
// of flags) ignores entity/seat and vacates whatever seat the peer holds. A join names the target
// aircraft by {entityIdx, entityGen} and the seat by seatIndex.
struct MsgSeatRequest {
    uint8_t msgId{static_cast<uint8_t>(MsgId::SeatRequest)}; // @0
    uint8_t flags{0};                                        // @1 bit 0 = leave (vacate current seat)
    uint8_t seatIndex{0};                                    // @2 seat to claim (ignored on leave)
    uint8_t reserved{0};                                     // @3 pad
    uint32_t entityIdx{0};                                   // @4 target aircraft entity index
    uint32_t entityGen{0};                                   // @8 target aircraft generation
}; // 12 bytes, align 4
static_assert(sizeof(MsgSeatRequest) == 12u, "MsgSeatRequest wire size changed");
static_assert(alignof(MsgSeatRequest) == 4u, "MsgSeatRequest alignment changed");
static_assert(offsetof(MsgSeatRequest, entityIdx) == 4u, "MsgSeatRequest::entityIdx offset changed");
static_assert(offsetof(MsgSeatRequest, entityGen) == 8u, "MsgSeatRequest::entityGen offset changed");

inline constexpr uint8_t kSeatRequestFlagLeave = 0x01u;

// Server->client reply to a MsgSeatRequest. Echoes the target so the client can correlate; on a grant
// the client also receives a fresh MsgConnectAck (assigned entity = the host aircraft) and the roster.
struct MsgSeatResult {
    uint8_t msgId{static_cast<uint8_t>(MsgId::SeatResult)}; // @0
    uint8_t code{0};                                        // @1 SeatResultCode
    uint8_t seatIndex{0};                                   // @2 echoed requested seat
    uint8_t reserved{0};                                    // @3 pad
    uint32_t entityIdx{0};                                  // @4 echoed target aircraft index
    uint32_t entityGen{0};                                  // @8 echoed target aircraft generation
}; // 12 bytes, align 4
static_assert(sizeof(MsgSeatResult) == 12u, "MsgSeatResult wire size changed");
static_assert(alignof(MsgSeatResult) == 4u, "MsgSeatResult alignment changed");
static_assert(offsetof(MsgSeatResult, entityIdx) == 4u, "MsgSeatResult::entityIdx offset changed");
static_assert(offsetof(MsgSeatResult, entityGen) == 8u, "MsgSeatResult::entityGen offset changed");

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
    // --- appended at the tail (#996); additive, prior offsets unchanged ---
    // Player callsign. A FIXED field (not a TLV) because every client always has one (PilotProfile::
    // callsign, default "Pilot") — TLVs in this message are for conditional payloads (seat claim,
    // reconnect identity, join password). The server sanitizes it (printable, trimmed, empty ->
    // "Pilot-<peerId>"). It is the client's contribution to the match roster (#996).
    char callsign[32]{}; // null-terminated display callsign; empty = server assigns a default
}; // 104 bytes, align 2 (multiple of alignof(PackManifestEntry)=1 so trailing records stay aligned)
static_assert(sizeof(MsgConnectRequest) == 104u, "MsgConnectRequest wire size changed");
static_assert(alignof(MsgConnectRequest) == 2u, "MsgConnectRequest alignment changed");
static_assert(offsetof(MsgConnectRequest, protocolVersion) == 2u, "MsgConnectRequest::protocolVersion offset changed");
static_assert(offsetof(MsgConnectRequest, packCount) == 4u, "MsgConnectRequest::packCount offset changed");
static_assert(offsetof(MsgConnectRequest, requestedEntityType) == 8u,
              "MsgConnectRequest::requestedEntityType offset changed");
static_assert(offsetof(MsgConnectRequest, callsign) == 72u, "MsgConnectRequest::callsign offset changed");

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
// MsgClientInput::buttons bit assignments.
inline constexpr uint8_t kInputButtonGun = 0x01;         // bit 0 = gun trigger (level)
inline constexpr uint8_t kInputButtonAfterburner = 0x02; // bit 1 = afterburner
inline constexpr uint8_t kInputButtonFireStore = 0x04;   // bit 2 = fire selected store (edge)
inline constexpr uint8_t kInputButtonChaffFlare = 0x08;  // bit 3 = chaff/flare dispense (edge, #529)
inline constexpr uint8_t kInputButtonEcm = 0x10;         // bit 4 = ECM jammer (level, #529)
inline constexpr uint8_t kInputButtonEject = 0x20;       // bit 5 = eject (edge, #672)
inline constexpr uint8_t kInputButtonWheelBrake = 0x40;  // bit 6 = wheel brakes (level, #700, ground only)
inline constexpr uint8_t kInputButtonRespawn = 0x80;     // bit 7 = respawn request (edge, #648) — LAST free
                                                         // bit; the next button must open a uint16 field.

struct MsgClientInput {
    uint8_t msgId{static_cast<uint8_t>(MsgId::ClientInput)};
    uint8_t buttons{0}; // bit 0 gun, bit 1 afterburner, bit 2 fire store, bit 3 chaff/flare, bit 4 ECM,
                        // bit 5 = eject (server edge-detects; a held key is one ejection, #672),
                        // bit 6 = wheel brakes (level; the integrator only brakes in ground contact, #700)
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

    // Radar operating mode (#526). ABSOLUTE, same rationale as selectedStation: the client sends the
    // mode it wants (Silent/Search/TWS/STT = 0..3), so it converges on the unreliable channel. 255 =
    // keep the current server-side mode (the default from an unaware client, so old clients and the
    // load bots leave the radar in its spawned Search mode). @49.
    uint8_t radarMode{255};
    uint8_t reservedB[2]{}; // explicit padding — the layout rule forbids implicit holes
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
static_assert(offsetof(MsgClientInput, radarMode) == 49u, "MsgClientInput::radarMode offset changed");
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
    double utcJulianDay{0.0}; // #481: the shared UTC clock (date + fractional time-of-day) as a Julian
                              // Day. The client combines it with its own camera latitude/longitude to
                              // compute the geographic sun, so the terminator moves across longitudes
                              // and two players far apart see different local suns. timeOfDayTenths
                              // remains the coarse HUD clock. Tail-append, additive; no version bump.
}; // 32 bytes, align 8
static_assert(sizeof(MsgWeatherState) == 32u, "MsgWeatherState wire size changed");
static_assert(offsetof(MsgWeatherState, turbulenceAmp) == 20u, "MsgWeatherState::turbulenceAmp offset changed");

// Music-state transition request (#413/#166): the server broadcasts this when a Lua/mission script
// calls world.set_music_state(). `state` is a GameState ordinal (Menu/FlightPatrol/FlightCombat/
// MissionSuccess/Debrief); the client maps it back and drives MusicManager::setState. Reliable so the
// transition is not lost. Additive id, old clients discard.
struct MsgMusicState {
    uint8_t msgId{static_cast<uint8_t>(MsgId::MusicState)};
    uint8_t state{0}; // GameState ordinal
    uint16_t reserved{0};
}; // 4 bytes, align 2
static_assert(sizeof(MsgMusicState) == 4u, "MsgMusicState wire size changed");

// Scripted haptic feedback (#128): a Lua script's rumble()/rumble_triggers()/stop_rumble() reaches the
// client as this message; the client plays it on its local gamepad (id 0 = the current player). The
// engine binding clamps a/b to [0,1] and durationMs to [0, 5000] before it is sent.
enum class HapticKind : uint8_t { Rumble = 0, Triggers = 1, Stop = 2 };
[[nodiscard]] inline bool isHapticKindOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(HapticKind::Stop);
}
struct MsgHaptic {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Haptic)};
    uint8_t kind{0};        // HapticKind
    uint16_t durationMs{0}; // clamped to [0, 5000]
    float a{0.f};           // Rumble: low-freq motor; Triggers: left trigger; Stop: unused
    float b{0.f};           // Rumble: high-freq motor; Triggers: right trigger; Stop: unused
}; // 12 bytes, align 4
static_assert(sizeof(MsgHaptic) == 12u, "MsgHaptic wire size changed");

// The mission's terminal outcome (#584): broadcast once when the objective evaluator drives the
// mission to Complete/Failed, so the client debrief reports the real result instead of a hardcoded
// success. `outcome` is a MissionResultCode.
enum class MissionResultCode : uint8_t { Incomplete = 0, Success = 1, Failure = 2 };
[[nodiscard]] inline bool isMissionResultOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(MissionResultCode::Failure);
}
struct MsgMissionOutcome {
    uint8_t msgId{static_cast<uint8_t>(MsgId::MissionOutcome)};
    uint8_t outcome{0}; // MissionResultCode
    uint16_t triggersFired{0};
    float elapsedSeconds{0.f};
}; // 8 bytes, align 4
static_assert(sizeof(MsgMissionOutcome) == 8u, "MsgMissionOutcome wire size changed");
static_assert(offsetof(MsgWeatherState, utcJulianDay) == 24u, "MsgWeatherState::utcJulianDay offset changed");
static_assert(alignof(MsgWeatherState) == 8u, "MsgWeatherState alignment changed");
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
    WeaponFired = 0,           // gunfire: tracer/muzzle at pos, srcIdx = shooter
    MissileLaunch = 1,         // store left the rails: srcIdx = shooter
    Impact = 2,                // a round connected: pos = hit, tgtIdx = struck entity
    Detonation = 3,            // warhead burst at pos
    NuclearFlash = 4,          // full-screen flash cue; pos = ground zero
    CountermeasureRelease = 5, // chaff/flare dispensed (#529): srcIdx = dispensing aircraft, pos = its position
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

// A "participant" (#497) is anything that occupies a scoreboard row: a human (participantId == its
// transport peerId) or a server-side AI bot (participantId == kBotParticipantBase + n). The two id
// spaces never overlap because ENet/GNS peer ids are small. CombatEventRecord's a/b fields (kill
// credit) and the server's per-participant score map both key on participantId, so a bot kill resolves
// to a name instead of kNoOwningPeer. The client resolves participantId -> callsign via the roster.
inline constexpr uint32_t kBotParticipantBase = 0x40000000u;
inline bool isBotParticipant(uint32_t participantId) noexcept {
    return participantId >= kBotParticipantBase && participantId != kNoOwningPeer;
}

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
// Match roster (#996)
// ---------------------------------------------------------------------------------------------
// PlayerRosterEntry::flags bits.
inline constexpr uint8_t kRosterLeave = 0x01u; // this participant left; the client removes the row
inline constexpr uint8_t kRosterBot = 0x02u;   // this participant is an AI bot (badge it, ping 0)

// Header of a PlayerRoster upsert/leave stream (#996). Followed by `count` PlayerRosterEntry records.
// A join/change is a one-entry broadcast; a leave is one entry with kRosterLeave; the full roster sent
// to a late joiner after ConnectAck is `count` upserts chunked <= kMaxRosterEntriesPerPacket. Dedup is
// by participantId (no full/delta distinction). Self-describing like MsgFactionDef, with a count header.
struct MsgPlayerRosterHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::PlayerRoster)}; // @0
    uint8_t count{0};                                         // @1 number of trailing PlayerRosterEntry records
    uint16_t reserved{0};                                     // @2 pad (keeps records 4-aligned)
}; // 4 bytes, align 2
static_assert(sizeof(MsgPlayerRosterHeader) == 4u, "MsgPlayerRosterHeader wire size changed");
static_assert(offsetof(MsgPlayerRosterHeader, count) == 1u, "MsgPlayerRosterHeader::count offset changed");

struct PlayerRosterEntry {
    uint32_t participantId{0}; // @0 peerId, or kBotParticipantBase + n for a bot
    uint16_t factionIndex{0};  // @4 FactionRegistry index (the participant's team); 0 = neutral/none
    uint8_t role{0};           // @6 PeerRole ordinal (bots report Pilot)
    uint8_t flags{0};          // @7 kRosterLeave / kRosterBot
    char callsign[32]{};       // @8 null-terminated display name
}; // 40 bytes, align 4
static_assert(sizeof(PlayerRosterEntry) == 40u, "PlayerRosterEntry wire size changed");
static_assert(alignof(PlayerRosterEntry) == 4u, "PlayerRosterEntry alignment changed");
static_assert(offsetof(PlayerRosterEntry, factionIndex) == 4u, "PlayerRosterEntry::factionIndex offset changed");
static_assert(offsetof(PlayerRosterEntry, role) == 6u, "PlayerRosterEntry::role offset changed");
static_assert(offsetof(PlayerRosterEntry, flags) == 7u, "PlayerRosterEntry::flags offset changed");
static_assert(offsetof(PlayerRosterEntry, callsign) == 8u, "PlayerRosterEntry::callsign offset changed");
inline constexpr std::size_t kMaxRosterEntriesPerPacket = 12; // 4 + 12*40 = 484 B (matches CombatEvent chunking)

// Client->server, reliable: request a mid-match switch to a different team/faction (#522). The server
// validates it against the balance guard (a switch that would stack a team is denied with a
// MsgServerNotice) before despawning + respawning the pilot on the new team. An admin `team` command
// bypasses the guard server-side.
struct MsgTeamRequest {
    uint8_t msgId{static_cast<uint8_t>(MsgId::TeamRequest)}; // @0
    uint8_t reserved{0};                                     // @1
    uint16_t factionIndex{0};                                // @2 requested destination team (FactionRegistry index)
}; // 4 bytes, align 2
static_assert(sizeof(MsgTeamRequest) == 4u, "MsgTeamRequest wire size changed");
static_assert(offsetof(MsgTeamRequest, factionIndex) == 2u, "MsgTeamRequest::factionIndex offset changed");

// Server->client, reliable: the current match phase, limits, and per-team scores (#523). Broadcast on
// change (phase transition or a team score) and unicast to a late joiner after ConnectAck. Followed by
// teamCount MatchTeamScore records. The client renders the phase clock as (phaseEndTick - tickIndex);
// phaseEndTick == 0 = the phase is untimed.
struct MsgMatchState {
    uint8_t msgId{static_cast<uint8_t>(MsgId::MatchState)}; // @0
    uint8_t phase{0};                                       // @1 MatchPhase ordinal
    uint16_t scoreLimit{0};                                 // @2 team score that ends the match; 0 = none
    uint8_t teamCount{0};                                   // @4 trailing MatchTeamScore records
    uint8_t reserved[3]{};                                  // @5 pad so phaseEndTick is 8-aligned
    uint64_t phaseEndTick{0};                               // @8 tick the current phase ends; 0 = untimed
    char modeId[32]{};                                      // @16 game-mode id
    char modeName[32]{};                                    // @48 game-mode display name
}; // 80 bytes, align 8
static_assert(sizeof(MsgMatchState) == 80u, "MsgMatchState wire size changed");
static_assert(alignof(MsgMatchState) == 8u, "MsgMatchState alignment changed");
static_assert(offsetof(MsgMatchState, scoreLimit) == 2u, "MsgMatchState::scoreLimit offset changed");
static_assert(offsetof(MsgMatchState, teamCount) == 4u, "MsgMatchState::teamCount offset changed");
static_assert(offsetof(MsgMatchState, phaseEndTick) == 8u, "MsgMatchState::phaseEndTick offset changed");
static_assert(offsetof(MsgMatchState, modeId) == 16u, "MsgMatchState::modeId offset changed");
static_assert(offsetof(MsgMatchState, modeName) == 48u, "MsgMatchState::modeName offset changed");

struct MatchTeamScore {
    uint16_t factionIndex{0}; // @0
    uint16_t reserved{0};     // @2
    int32_t score{0};         // @4
}; // 8 bytes, align 4
static_assert(sizeof(MatchTeamScore) == 8u, "MatchTeamScore wire size changed");
static_assert(offsetof(MatchTeamScore, score) == 4u, "MatchTeamScore::score offset changed");

// Server->client, unreliable: the full scoreboard (#523). Periodic + on admit; followed by `count`
// ScoreboardRow records (chunked). Unreliable because it is fully self-describing and refreshed every
// ~2 s — a lost scoreboard is replaced, not lost state.
struct MsgScoreboardHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Scoreboard)}; // @0
    uint8_t count{0};                                       // @1 trailing ScoreboardRow records
    uint16_t reserved{0};                                   // @2
    uint32_t reserved2{0};                                  // @4 pad so records stay 4-aligned
}; // 8 bytes, align 4
static_assert(sizeof(MsgScoreboardHeader) == 8u, "MsgScoreboardHeader wire size changed");
static_assert(offsetof(MsgScoreboardHeader, count) == 1u, "MsgScoreboardHeader::count offset changed");

struct ScoreboardRow {
    uint32_t participantId{0}; // @0 peerId, or kBotParticipantBase + n
    int32_t score{0};          // @4
    uint16_t kills{0};         // @8
    uint16_t deaths{0};        // @10
    uint16_t pingMs{0};        // @12 estimatedDelayTicks*1000/60, capped; 0 for bots
    uint16_t factionIndex{0};  // @14
}; // 16 bytes, align 4
static_assert(sizeof(ScoreboardRow) == 16u, "ScoreboardRow wire size changed");
static_assert(offsetof(ScoreboardRow, kills) == 8u, "ScoreboardRow::kills offset changed");
static_assert(offsetof(ScoreboardRow, pingMs) == 12u, "ScoreboardRow::pingMs offset changed");
static_assert(offsetof(ScoreboardRow, factionIndex) == 14u, "ScoreboardRow::factionIndex offset changed");
inline constexpr std::size_t kMaxScoreboardRowsPerPacket = 30; // 8 + 30*16 = 488 B

// ---------------------------------------------------------------------------------------------
// Datalink / shared team track picture (#528)
// ---------------------------------------------------------------------------------------------
// The fused picture a peer's TEAM can see: the peer's own sensor contacts PLUS every same-faction
// teammate's contacts, merged by target. This is what a coordinated battle turns on — you see the
// bandit your wingman locked even if your own radar never found it. Sent per-peer, UNRELIABLE, at
// ~6 Hz (a dropped frame just means one stale refresh). Positions are float, relative to the header
// `origin` (the peer's aircraft, or an observer's camera), so the whole picture stays float-precise
// at any world scale and an observer with no aircraft still has a frame of reference.
//
// The client already has entity positions from the snapshot, but a datalink track may be a target
// its OWN sensors never saw and that interest management culled from its snapshot entirely — so the
// datalink carries position itself rather than assuming the client can look it up.

inline constexpr std::size_t kMaxDatalinkTracks = 48;  // bounds the per-peer wire cost at scale
inline constexpr std::size_t kMaxDatalinkThreats = 16; // one RWR does not show a hundred strobes

inline constexpr uint8_t kDatalinkFlagFiringQuality = 0x01u; // a firing-quality (STT) lock is on it
inline constexpr uint8_t kDatalinkFlagOwnSensor = 0x02u;     // THIS peer's own sensors hold it (not only datalink)

struct MsgDatalinkHeader {
    uint8_t msgId{static_cast<uint8_t>(MsgId::Datalink)};
    uint8_t flags{0};
    uint16_t trackCount{0};  // DatalinkTrack records following the header
    uint16_t threatCount{0}; // DatalinkThreat records following the tracks
    uint16_t reserved{0};
    uint64_t tickIndex{0};
    double origin[3]{}; // world position the relative track/threat positions are measured from
}; // 40 bytes, align 8
static_assert(sizeof(MsgDatalinkHeader) == 40u, "MsgDatalinkHeader wire size changed");
static_assert(alignof(MsgDatalinkHeader) == 8u, "MsgDatalinkHeader alignment changed");
static_assert(offsetof(MsgDatalinkHeader, trackCount) == 2u, "MsgDatalinkHeader::trackCount offset changed");
static_assert(offsetof(MsgDatalinkHeader, threatCount) == 4u, "MsgDatalinkHeader::threatCount offset changed");
static_assert(offsetof(MsgDatalinkHeader, tickIndex) == 8u, "MsgDatalinkHeader::tickIndex offset changed");
static_assert(offsetof(MsgDatalinkHeader, origin) == 16u, "MsgDatalinkHeader::origin offset changed");

// One fused track. `state` = ContactState, `ident` = Identification, `sensorTypeMask` = which kinds
// of sensor hold it across the team. Position/velocity are relative to MsgDatalinkHeader::origin.
struct DatalinkTrack {
    uint32_t targetIdx{0};
    uint32_t typeIndex{0};
    uint16_t targetGen{0};
    uint16_t factionIndex{0}; // the target's actual faction (the client colours by `ident`, not this)
    uint8_t state{0};         // ContactState ordinal
    uint8_t ident{0};         // Identification ordinal (Friend/Foe/Unknown) — the display-safe fact
    uint8_t sensorTypeMask{0};
    uint8_t flags{0}; // kDatalinkFlag*
    float relPos[3]{};
    float relVel[3]{};
}; // 40 bytes, align 4
static_assert(sizeof(DatalinkTrack) == 40u, "DatalinkTrack wire size changed");
static_assert(alignof(DatalinkTrack) == 4u, "DatalinkTrack alignment changed");
static_assert(offsetof(DatalinkTrack, targetGen) == 8u, "DatalinkTrack::targetGen offset changed");
static_assert(offsetof(DatalinkTrack, state) == 12u, "DatalinkTrack::state offset changed");
static_assert(offsetof(DatalinkTrack, relPos) == 16u, "DatalinkTrack::relPos offset changed");
static_assert(offsetof(DatalinkTrack, relVel) == 28u, "DatalinkTrack::relVel offset changed");

// One RWR threat: an emitter painting this peer. Position is relative to MsgDatalinkHeader::origin.
struct DatalinkThreat {
    uint32_t emitterIdx{0};
    uint32_t emitterTypeIndex{0};
    uint16_t emitterGen{0};
    uint16_t emitterFactionIndex{0};
    uint8_t channel{0}; // SensorType ordinal (radar / laser)
    uint8_t level{0};   // ThreatLevel ordinal (search strobe / lock tone)
    uint8_t ident{0};   // Identification of the emitter (#527 correlation): friendly emitters are benign
    uint8_t flags{0};
    float relPos[3]{};
}; // 28 bytes, align 4
static_assert(sizeof(DatalinkThreat) == 28u, "DatalinkThreat wire size changed");
static_assert(alignof(DatalinkThreat) == 4u, "DatalinkThreat alignment changed");
static_assert(offsetof(DatalinkThreat, emitterGen) == 8u, "DatalinkThreat::emitterGen offset changed");
static_assert(offsetof(DatalinkThreat, channel) == 12u, "DatalinkThreat::channel offset changed");
static_assert(offsetof(DatalinkThreat, relPos) == 16u, "DatalinkThreat::relPos offset changed");

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

// One generic player radio command (#703), client->server reliable. Shared by ATC now and reusable by
// other radio grammars later. Verb-routed like the admin channel — no direct state mutation. The
// server dispatches the verb to the ATC service and replies with MsgRadioTransmission(s).
struct MsgRadioCommand {
    uint8_t msgId{static_cast<uint8_t>(MsgId::RadioCommand)};
    uint8_t reserved{0};
    uint16_t reqId{0};  // client-generated correlation id (diagnostic; echoing is optional)
    char command[60]{}; // null-terminated verb string, e.g. "atc request_landing khjo"; 59 usable
}; // 64 bytes, align 2
static_assert(sizeof(MsgRadioCommand) == 64u, "MsgRadioCommand wire size changed");
static_assert(alignof(MsgRadioCommand) == 2u, "MsgRadioCommand alignment changed");
static_assert(offsetof(MsgRadioCommand, reqId) == 2u, "MsgRadioCommand::reqId offset changed");
static_assert(offsetof(MsgRadioCommand, command) == 4u, "MsgRadioCommand::command offset changed");

// One spoken radio line (#703), server->client reliable, unicast (directed) or broadcast. The text is
// server-rendered + localizable; voiceKey is a stable key for TTS / a content-pack OGG (empty = no
// audio, subtitle only — the docs/ai-architecture.md degradation path).
struct MsgRadioTransmission {
    uint8_t msgId{static_cast<uint8_t>(MsgId::RadioTransmission)};
    uint8_t reserved{0};
    uint16_t displaySeconds{6}; // subtitle dwell
    char speaker[28]{};         // e.g. "Riverside Tower"; 27 usable
    char voiceKey[32]{};        // stable TTS / OGG key; 31 usable; empty = subtitle only
    char text[160]{};           // rendered line; 159 usable
}; // 224 bytes, align 2
static_assert(sizeof(MsgRadioTransmission) == 224u, "MsgRadioTransmission wire size changed");
static_assert(alignof(MsgRadioTransmission) == 2u, "MsgRadioTransmission alignment changed");
static_assert(offsetof(MsgRadioTransmission, displaySeconds) == 2u, "MsgRadioTransmission::displaySeconds offset");
static_assert(offsetof(MsgRadioTransmission, speaker) == 4u, "MsgRadioTransmission::speaker offset changed");
static_assert(offsetof(MsgRadioTransmission, voiceKey) == 32u, "MsgRadioTransmission::voiceKey offset changed");
static_assert(offsetof(MsgRadioTransmission, text) == 64u, "MsgRadioTransmission::text offset changed");

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
    uint8_t gameModeFlags{0};    // see kGameMode* constants
    uint8_t reserved2{0};        // @9
    uint16_t shutdownSeconds{0}; // @10 seconds until shutdown when kGameModeShuttingDown is set (#226); 0 = n/a
    char name[64]{};             // @12 null-terminated server name
}; // 76 bytes, align 2
static_assert(sizeof(MsgLanBeacon) == 76u, "MsgLanBeacon wire size changed");
static_assert(alignof(MsgLanBeacon) == 2u, "MsgLanBeacon alignment changed");
static_assert(offsetof(MsgLanBeacon, protocolVersion) == 2u, "MsgLanBeacon::protocolVersion offset changed");
static_assert(offsetof(MsgLanBeacon, gamePort) == 4u, "MsgLanBeacon::gamePort offset changed");
static_assert(offsetof(MsgLanBeacon, playerCount) == 6u, "MsgLanBeacon::playerCount offset changed");
static_assert(offsetof(MsgLanBeacon, maxPlayers) == 7u, "MsgLanBeacon::maxPlayers offset changed");
static_assert(offsetof(MsgLanBeacon, gameModeFlags) == 8u, "MsgLanBeacon::gameModeFlags offset changed");
static_assert(offsetof(MsgLanBeacon, shutdownSeconds) == 10u, "MsgLanBeacon::shutdownSeconds offset changed");
static_assert(offsetof(MsgLanBeacon, name) == 12u, "MsgLanBeacon::name offset changed");

// Bitmask constants for MsgLanBeacon::gameModeFlags.
static constexpr uint8_t kGameModeCampaign = 0x01u;
static constexpr uint8_t kGameModeMission = 0x02u;
static constexpr uint8_t kGameModeSandbox = 0x04u;
static constexpr uint8_t kGameModeShuttingDown = 0x08u; // the server is counting down to shutdown (#226)

// Extension tag registry for TLV blocks appended after fixed message structs (see WireCodec.h).
// Wire format per entry: [tag: uint16_t LE][len: uint16_t LE][data: len bytes].
// Senders include any subset; receivers skip unknown tags via their len field.
// Range layout:
//   0x0100–0x01FF  MsgWorldSnapshot extensions (appended after the quantized record bitstream)
//   0x0200–0x02FF  MsgConnectAck extensions (reserved for future use)
//   0x0300–0x03FF  MsgClientInput extensions (reserved for future use)
//   0x0400–0x04FF  MsgWeatherState extensions (reserved for future use)
//   0x0500–0x05FF  MsgConnectRequest extensions (0x0500 = ConnectSeatClaim #974; RFC #871 token reserved)
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
        0x0105,            // uint32_t: seqNum of the last MsgClientInput the server drained + APPLIED for the receiving
                           // peer (#427). Lets client prediction replay EXACTLY the inputs the server has not yet
                           // reflected (history seqNum > this), instead of approximating the replay depth from
                           // estimatedDelayTicks. Omitted until the first input is applied (a peer's first snapshots).
    SnapshotCrew = 0x0106, // #972: live turret orientation for CREWED aircraft in the peer's interest set.
                           // Payload: uint8 entryCount, then entryCount x { uint32 entityIdx (LE), uint8
                           // turretCount, turretCount x { int16 azQ (LE), int16 elQ (LE) } }. az quantized
                           // over [-pi,pi], el over [-pi/2,pi/2] to int16. Single-seat aircraft NEVER appear
                           // (occupancy lives in the reliable MsgCrewRoster), so a world of only single-seat
                           // entities emits no SnapshotCrew TLV and its snapshot is byte-identical to pre-#972.
                           // Unreliable/interest-filtered: a dropped packet loses one tick of turret aim.

    ConnectSeatClaim = 0x0500,   // #974: join-at-connect seat claim in MsgConnectRequest's TLV block.
                                 // Payload = { uint32 entityIdx (LE), uint32 entityGen (LE), uint8 seatIndex }
                                 // (9 bytes, unaligned). Present = the client asks to occupy that non-fly seat
                                 // instead of spawning its own aircraft; the server falls back to a normal
                                 // pilot spawn if the seat is unavailable. Absent = the normal pilot spawn.
    WeatherWindProfile = 0x0400, // #489: altitude wind profile appended to MsgWeatherState. Payload =
                                 // uint8 count + count x {float altM, float windX, float windZ} (12 B each,
                                 // little-endian, unaligned). Ascending altitude. Old clients ignore it and
                                 // keep the datum-level windX/windZ scalar; omitted when no profile is set.
};

} // namespace fl
