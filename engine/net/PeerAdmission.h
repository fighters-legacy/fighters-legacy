// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GameProtocol.h"       // PeerRole, ConnectRefusalCode, the connect-path messages
#include "IClock.h"             // injectable clock behind the per-IP connect-rate window
#include "RequiredPackPolicy.h" // RequiredPack + the missing-pack policy (#872)
#include "entity/EntityId.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fl {

class EntityManager;
class EntityTypeRegistry;
class ILogger;
class INetwork;
class WorldBroadcaster;
struct WorldBroadcasterHooks; // engine/net/WorldBroadcaster.h — the host seams (#1082, D12)
struct WorldQueries;

// Sentinel initial airspeed (#883): "no explicit speed given — pick a sane cruise default for an
// airborne spawn." A concrete value (incl. 0 for a ground start, #885) is used verbatim. Any
// negative value is treated as auto.
inline constexpr float kAutoSpawnAirspeed = -1.f;

// Sentinel for "no specific team preference" in claimMissionSlot / assignment (#522).
inline constexpr uint16_t kNoFaction = 0xFFFFu;

// One joinable player slot authored by a mission (#854). The slot pins WHERE and WHAT a joining
// pilot flies; the team balancer stays authoritative about WHICH SIDE (#522).
struct MissionSpawnSlot {
    std::string missionObjectId; // the mission object id this slot came from (#884), reported to
                                 // the mission-slot binder so destroy(<id>) tracks the pilot
    std::string entityType;
    uint16_t factionIndex{0};
    double pos[3]{};
    float quat[4]{0.f, 0.f, 0.f, 1.f};
    float airspeed{kAutoSpawnAirspeed}; // initial airspeed for the joining pilot (#883); auto = cruise
    // The stores the joining pilot takes off with (#1209), in hardpoint order and in the mission's
    // own vocabulary ("~"/"-"/"" = empty station; short lists leave later stations at their default).
    // Empty = the entity def's default fit, which is what every slot had before this existed.
    std::vector<std::string> loadout;
};

// Whether and how a peer enters the world (#1085, D13).
//
// Extracted from WorldBroadcaster as a PURE relocation: the five-gate connect gauntlet, the
// MsgConnectRequest handshake end to end, the ConnectAck reply burst, the mission player slots, the
// reconnection grace table and the refusal table all moved here unchanged, together with the state
// only they touch. The wire bytes are identical by construction — every send below is the same call
// in the same order it was made from inside the broadcaster.
//
// A CONCRETE class owned by value, not an interface (D13): there is no second implementation, and
// the tests exercise this class directly. An interface can be cut later if a real one appears.
//
// The `WorldBroadcaster&` back-reference is the deliberate remaining seam. Admission is inherently
// world-mutating — it spawns airframes, binds crew seats, writes the roster and the scoreboard — and
// that machinery stays in the broadcaster. Rather than re-expose twenty broadcaster internals as
// public methods (the 205-method surface this epic exists to shrink) or re-hook them as twenty
// std::functions (the pattern this epic exists to delete), this class is a friend of its owner and
// calls them directly. Narrowing that seam is behavioural work and belongs after the structure has
// settled, not inside a stage whose safety net is byte-identical output.
//
// Sim-thread only, like the broadcaster that owns it.
class PeerAdmission {
  public:
    // `wb` owns this object by value and outlives it. `queries`/`hooks` are the broadcaster's own
    // frozen seam members, bound by reference: PeerAdmission is declared after them, so they are
    // fully constructed before this reference is formed.
    PeerAdmission(WorldBroadcaster& wb, EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                  ILogger& logger, const WorldQueries& queries, const WorldBroadcasterHooks& hooks) noexcept;

    // ── the connect path ────────────────────────────────────────────────────
    // The rejection gauntlet (ban → allowlist → per-IP connect rate → per-IP concurrent cap → admin
    // auth lockout), MsgHello, and the peer's input slot. Admission proper waits for the request.
    void onConnect(uint32_t peerId);
    // Handle MsgConnectRequest (#853): version backstop, duplicate guard, join password, required-pack
    // policy, role grant, seat-claim/identity TLVs, reconnect grace, team assignment, spawn hand-off,
    // ConnectAck, MOTD, flight check-in, roster and scoreboard.
    void handleConnectRequest(uint32_t peerId, const void* data, std::size_t size);
    // The disconnect cleanup of THIS object's maps: snapshot the peer's identity + tallies under its
    // client guid for the reconnect grace window (#524), then drop the guid. Called from
    // WorldBroadcaster::onDisconnect at the position the inlined block held.
    void onDisconnect(uint32_t peerId);

    // Re-spawn every connected Pilot peer that currently has no aircraft (after resetWorld).
    void readmitPilots();

    // The ConnectAck reply burst: the ack itself (type table skipped when the peer's generation is
    // current, #1070), the faction table + alert levels, the radio-net defs, the mission roster and
    // every crewed aircraft's seat roster. Re-sent on seat / role / team / authority changes, which is
    // why the broadcaster calls it from outside the connect path too.
    void sendConnectAck(uint32_t peerId, EntityId assigned, PeerRole grantedRole);
    void sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason);
    // Log the refusal, send MsgConnectRefusal with the matching reason, and disconnect. The one place
    // the refusal table is consulted.
    void rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code);

    // Spawn a pilot peer's entity of `entityType` at a round-robin spawn point and stamp its team.
    // `faction` kNoFaction ⇒ the configured player faction. Returns an invalid id on spawn failure.
    EntityId admitPilot(uint32_t peerId, const std::string& entityType, uint16_t faction = kNoFaction);
    // Resolve the entity type to spawn for a pilot (#834): a client-requested type wins iff it is a
    // REGISTERED type (server-clamped allowlist); otherwise the [world] player_entity_type default;
    // otherwise builtin:debug-entity. An unregistered request falls back with an Info log.
    [[nodiscard]] std::string resolvePlayerEntityType(const char* requested) const;

    // ── mission player slots (#854) ─────────────────────────────────────────
    void setMissionPlayerSlots(std::vector<MissionSpawnSlot> slots);
    // Claim the next open slot for `peerId`; -1 when there are none or all are occupied.
    // `preferredFaction` (kNoFaction = any) prefers a slot on that team, falling back to any open one.
    int claimMissionSlot(uint32_t peerId, uint16_t preferredFaction = kNoFaction);
    void releaseMissionSlot(uint32_t peerId);

    // ── periodic maintenance ────────────────────────────────────────────────
    // Coarse prune of stale connect-rate records, admin-auth lockouts and expired reconnect grace
    // entries. Called from the broadcaster's 600-tick maintenance block; the three prunes stay in one
    // function so their relative order is preserved exactly.
    void pruneStaleRecords(uint64_t currentTick);
    void clearDisconnectGrace() noexcept; // #524: scores belong to a match; a new round starts clean

    // ── configuration (the broadcaster's public setters forward here) ───────
    void banAddress(std::string ip);
    void unbanAddress(const std::string& ip);
    void setBannedAddresses(std::unordered_set<std::string> addrs);
    [[nodiscard]] std::unordered_set<std::string> bannedAddresses() const;
    void setAllowedAddresses(std::unordered_set<std::string> addrs);
    void setRateLimitParams(int maxConnects, int windowSeconds) noexcept;
    void setMaxConnectionsPerIp(int max) noexcept;
    void setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept;
    // Test seam (#1334): when >= 0, every admitPilot spawn uses exactly this initial airspeed
    // instead of the production rule (ramp points parked, airborne spawns at the default cruise).
    // Wire-instrument tests set 0 so their subject entity holds still while bytes are compared.
    void setSpawnAirspeedOverride(float mps) noexcept {
        m_spawnAirspeedOverride = mps;
    }
    void setPlayerFaction(uint16_t faction) noexcept;
    void setPlayerEntityType(std::string type);
    void setAllowObservers(bool allow) noexcept;
    void setRequiredPacks(std::vector<RequiredPack> packs);
    void setRequiredPackPolicy(RequiredPackPolicy policy) noexcept;
    void setJoinPassword(std::string password);
    void setReconnectGraceTicks(uint64_t ticks) noexcept;
    void setClock(const IClock& clock) noexcept;

  private:
    WorldBroadcaster& m_wb; // the world this admits into; see the class comment
    EntityManager& m_entityManager;
    EntityTypeRegistry& m_registry;
    INetwork& m_net;
    ILogger& m_logger;
    const WorldQueries& m_queries;
    const WorldBroadcasterHooks& m_hooks;

    // Injectable clock for testing; mirrors the broadcaster's (setClock forwards to both).
    const IClock* m_clock{&SystemClock::instance()};

    std::unordered_set<std::string> m_bannedAddresses;  // in-memory ban list
    std::unordered_set<std::string> m_allowedAddresses; // empty = allowlist disabled

    // Per-IP sliding-window connection rate limiter.
    struct ConnectRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };
    std::unordered_map<std::string, ConnectRecord> m_connectRecords;
    int m_connectRateLimit{5};
    int m_connectRateWindowS{10};
    int m_maxConnectionsPerIp{0}; // 0 = unlimited

    // ── reconnection (#524) ──────────────────────────────────────────────────
    struct GraceRec {
        std::string callsign;
        uint16_t factionIndex{0};
        uint32_t kills{0};
        uint32_t losses{0};
        int32_t score{0};
        uint64_t expiresTick{0};
    };
    std::unordered_map<std::string, GraceRec> m_disconnectGrace; // guid -> held identity/score
    std::unordered_map<uint32_t, std::string> m_peerGuids;       // peerId -> client guid (set at handshake)
    uint64_t m_reconnectGraceTicks{0};                           // 0 = disabled

    std::vector<std::array<double, 3>> m_spawnPoints;
    float m_spawnAirspeedOverride{
        -1.f}; // < 0 = production rule (#1334); >= 0 = forced (tests) // pre-cached [x,y,z]; read-only after start
    uint32_t m_nextSpawnIdx{0}; // round-robin counter

    // Mission player slots (#854). m_slotOccupant[i] = the peer holding slot i, or kSlotFree.
    // m_peerSlot maps a peer to its held slot for O(1) release on despawn.
    static constexpr uint32_t kSlotFree = 0xFFFFFFFFu;
    std::vector<MissionSpawnSlot> m_missionSlots;
    std::vector<uint32_t> m_slotOccupant;
    std::unordered_map<uint32_t, int> m_peerSlot;

    uint16_t m_playerFaction{1};                            // 0 restores the legacy neutral-player behavior
    std::string m_playerEntityType{"builtin:debug-entity"}; // pilot spawn default when client requests none (#834)
    bool m_allowObservers{true};                            // #857: false = refuse observer connect requests
    std::vector<RequiredPack> m_requiredPacks;              // #872: packs a client must have (id + optional version)
    RequiredPackPolicy m_requiredPackPolicy{RequiredPackPolicy::Warn}; // #872: what to do when one is missing
    std::string m_joinPassword;                                        // #998: empty = open server
};

} // namespace fl
