// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "CrewClientState.h" // client crew roster + turret pose (#972)
#include "IClock.h"
#include "INetwork.h"
#include "RenderTypes.h"
#include "SessionStatus.h"
#include "WingmanMenu.h"
#include "net/GameProtocol.h"      // PeerRole, PackManifestEntry (connect handshake #853)
#include "render/RadarView.h"      // RadarView / RadarTrack / RwrStrobe (datalink picture #528)
#include "render/RenderSnapshot.h" // EntityRenderEntry (stored by value in the retention cache)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class ClientEffectRouter;
class GameConsole;
class ILogger;
class ServerNotice;
struct RenderSnapshot;
struct MsgClientInput;
struct MsgHeartbeat;
class INetwork;

class EntityTypeRegistry;
class SimRenderBridge;

// Wall-clock render interpolation alpha, reset on each received WorldSnapshot.
// Replaces the in-process GameLoop::shellTick() that was removed with the
// embedded server.
struct ClientTickAlpha {
    std::chrono::steady_clock::time_point lastTick{std::chrono::steady_clock::now()};
    void markNewTick() noexcept {
        lastTick = std::chrono::steady_clock::now();
    }
    float get() const noexcept {
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastTick).count();
        return std::clamp(dt * 60.0f, 0.0f, 1.0f);
    }
};

// Parses ENet packets from the local fl-server subprocess and feeds them into
// the render bridge and environment state. Forwards server notices to the game
// console and the server notice overlay.
struct ClientNetEventHandler : INetworkEventHandler {
    SimRenderBridge& bridge;
    EntityTypeRegistry& registry;
    ILogger& logger;
    INetwork& net;
    EnvironmentState& env;                // updated on MsgWeatherState
    GameConsole* console{nullptr};        // optional: server notices are printed here
    ServerNotice* notice{nullptr};        // optional: server notices shown as screen banner
    WingmanMenu* wingman{nullptr};        // optional: flight check-in / order acks / relayed radio calls (#610)
    ClientEffectRouter* effects{nullptr}; // optional: cosmetic weapon effects (#625) — particles now, audio #631
    uint32_t motdDisplaySeconds{15};      // user-configurable; 0 = persistent

    uint32_t assignedEntityIdx{0};
    uint32_t assignedEntityGen{0};

    // Connect handshake inputs (#853). Set by Game::startGame() before net.connect(); sent as
    // MsgConnectRequest from onConnect. Defaults: pilot, server-chosen aircraft, no mounted packs.
    PeerRole requestedRole{PeerRole::Pilot};
    std::string requestedEntityType;             // empty = let the server pick its default (#834)
    std::vector<PackManifestEntry> packManifest; // client's mounted content packs (#872 wire half)

    // Role the server GRANTED in MsgConnectAck (#857) — may differ from requestedRole. Pilot until the
    // ack arrives. gotConnectAck() distinguishes an observer's valid (entity-less) ack from a pre-ack
    // rejection, which the old assignedEntityIdx==0 sentinel could not.
    PeerRole grantedRole() const noexcept {
        return m_grantedRole;
    }
    bool gotConnectAck() const noexcept {
        return m_gotConnectAck;
    }

    // Display name for a faction index (#860), from the MsgFactionDef table. Empty string if the
    // faction is unknown (no table received, or an out-of-range index).
    std::string factionName(uint16_t factionIndex) const {
        const auto it = m_factionNames.find(factionIndex);
        return it != m_factionNames.end() ? it->second : std::string{};
    }

    // The seat roster of a crewed aircraft (#972), or nullptr when the entity is single-seat / unknown.
    // Keyed by entity index; the stored gen guards against a pool-slot reuse applying a stale roster.
    const CrewRosterInfo* crewRoster(uint32_t entityIdx) const {
        const auto it = m_crewRosters.find(entityIdx);
        return it != m_crewRosters.end() ? &it->second : nullptr;
    }

    // Live mount-frame turret poses of a crewed aircraft from the last SnapshotCrew TLV (#972). Empty
    // when the entity is not crewed / not in the last snapshot's interest set. Main-thread only.
    std::span<const CrewTurretPose> crewTurretPoses(uint32_t entityIdx) const {
        const auto it = m_crewTurretPoses.find(entityIdx);
        return it != m_crewTurretPoses.end() ? std::span<const CrewTurretPose>(it->second.data(), it->second.size())
                                             : std::span<const CrewTurretPose>{};
    }

    // Every crewed aircraft roster the client knows (#975). The seat picker iterates these.
    const std::unordered_map<uint32_t, CrewRosterInfo>& crewRosters() const noexcept {
        return m_crewRosters;
    }

    // ── Seat join/leave protocol (#974/#975), client half ────────────────────────────────────────
    // Send a MsgSeatRequest to claim `seat` of the crewed aircraft {entityIdx, entityGen}, or to leave
    // the current seat. Reliable. The outcome arrives as MsgSeatResult (see takeSeatResult()).
    void sendSeatRequest(uint32_t entityIdx, uint32_t entityGen, uint8_t seat);
    void sendSeatLeave();

    // The outcome of the last MsgSeatRequest (#975), for the seat-picker UI to surface. `valid` is
    // false until the first result arrives; `fresh` marks a result not yet consumed by the UI.
    struct SeatResultView {
        bool valid{false};
        bool fresh{false};
        uint8_t code{0}; // fl::SeatResultCode
        uint8_t seatIndex{0};
        uint32_t entityIdx{0};
    };
    SeatResultView takeSeatResult() noexcept {
        SeatResultView v = m_lastSeatResult;
        m_lastSeatResult.fresh = false;
        return v;
    }
    const SeatResultView& lastSeatResult() const noexcept {
        return m_lastSeatResult;
    }

    // #975: true after this client successfully JOINED a non-fly crew seat (a gunner on someone else's
    // airframe) — the seat join protocol only ever grants NON-fly seats, so any granted join means "I
    // do not fly this aircraft." The pilot flight-prediction path is gated off it (a gunner does not
    // predict flight). A leave (→ observer) or a fresh connect clears it.
    [[nodiscard]] bool inCrewSeat() const noexcept {
        return m_inCrewSeat;
    }

    ClientTickAlpha tickAlpha;

    // Set by Game::startGame() after construction. When non-null, a typed failure is stored here
    // (first-writer-wins) so LoadingScreen::Phase::Connecting can surface it immediately.
    std::atomic<SessionFailure>* sessionFailure{nullptr};

    ClientNetEventHandler(SimRenderBridge& b, EntityTypeRegistry& r, ILogger& l, INetwork& n, EnvironmentState& e)
        : bridge(b), registry(r), logger(l), net(n), env(e) {}

    void onConnect(uint32_t peerId) override;
    void onDisconnect(uint32_t peerId) override;
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override;

    // Planet radius received from the server in MsgConnectAck (km).
    // Valid after MsgConnectAck is parsed; read from the main thread after connection.
    float planetRadiusKm() const noexcept {
        return m_planetRadiusKm;
    }

    // Shared UTC clock (Julian Day) from the last MsgWeatherState (#481). Combined with the camera's
    // latitude/longitude to compute the per-observer geographic sun each frame. 0 until the first
    // weather packet. Main-thread only (client onReceive and the render loop are both main-thread).
    double utcJulianDay() const noexcept {
        return m_utcJulianDay;
    }

    // Active connected peer count from the last received MsgWorldSnapshot TLV extension block
    // (ExtTag::SnapshotPeerCount). Returns 0 if no extended snapshot has been received yet.
    uint16_t serverPeerCount() const noexcept {
        return m_serverPeerCount.load(std::memory_order_relaxed);
    }

    // The datalink track picture + RWR from the last MsgDatalink (#528), for the HUD radar scope.
    // `valid` is false until the first datalink arrives. Main-thread only (built in onReceive, read in
    // the render loop, both main-thread). The spans stay valid until the next MsgDatalink.
    RadarView radarView() const noexcept {
        RadarView v;
        v.tracks = std::span<const RadarTrack>(m_radarTracks.data(), m_radarTracks.size());
        v.strobes = std::span<const RwrStrobe>(m_rwrStrobes.data(), m_rwrStrobes.size());
        v.valid = m_haveDatalink;
        return v;
    }

    // This session's combat tallies, as the SERVER counts them (#626) — updated from the unicast
    // Stats records on the CombatEvent channel. Zero until the first stat-changing event. Read on
    // the main thread for the debrief screen and the pilot profile; reset per session because the
    // handler is re-created per session (reinitFlight).
    struct SessionCombatStats {
        uint32_t kills{0};
        uint32_t losses{0};
        int32_t score{0};
        // Per-target-class kills THIS peer scored, indexed by ObjectCategory ordinal (#674). Fed from
        // the Kill records (victim classified via the type registry) so the debrief can write the
        // pilot logbook's per-class tally. Sums to at most `kills` (a kill with an unknown victim
        // type is still counted in `kills` but may miss a class here).
        uint32_t killsByClass[8]{};
    };
    const SessionCombatStats& sessionStats() const noexcept {
        return m_sessionStats;
    }

    // The mission's terminal outcome (#584), from MsgMissionOutcome. Incomplete until the objective
    // evaluator ends the mission; the debrief reads it so it stops hardcoding success. Main-thread only.
    [[nodiscard]] fl::MissionResultCode missionOutcome() const noexcept {
        return m_missionOutcome;
    }

    // Issue a monotonically incrementing request ID for the next MsgAdminCommand.
    // Each call increments the counter; wraps at uint16_t max (harmless — ENet ordering prevents
    // interleaving and the client does not enforce reqId matching in chunk reassembly).
    uint16_t issueReqId() noexcept {
        return m_nextReqId++;
    }

    // Inject a deterministic clock for testing (default: SystemClock).
    void setClock(const fl::IClock& clock) noexcept {
        m_clock = &clock;
    }

    // Send a MsgHeartbeat if at least 1 second has elapsed and at least one WorldSnapshot has
    // been received (guards against sending tickIndex=0 which yields a bogus server delay estimate).
    // Call once per frame from FlightScreen::update().
    void sendHeartbeatIfNeeded();

    // Stamp the snapshot ack (high-water tickIndex + selective-ack ackMask, #566) onto an outgoing
    // client->server message from this handler's tracked state. This handler is the single ack
    // authority — call before sending a MsgClientInput so tickIndex/ackMask stay consistent.
    void stampAck(MsgClientInput& in) const noexcept;
    void stampAck(MsgHeartbeat& hb) const noexcept;

    uint32_t lastRttMs() const noexcept {
        return m_lastRttMs;
    }
    bool hasRtt() const noexcept {
        return m_rttValid;
    }

    // Per-peer latency from the last received MsgWorldSnapshot SnapshotPeerLatency TLV extension.
    // Returns 0 until the first extended snapshot with a non-zero delay arrives.
    uint32_t snapshotLatencyMs() const noexcept {
        return m_snapshotLatencyMs;
    }
    bool hasSnapshotLatency() const noexcept {
        return m_hasSnapshotLatency;
    }

    // Optional: called after snapshot assembly, before publishExternal().
    // Args: (RenderSnapshot& snap, uint64_t tickIndex, uint32_t estimatedDelayTicks, uint32_t ackedSeqNum)
    // ackedSeqNum is the exact seqNum the server last applied for this peer (SnapshotLastAckedSeqNum
    // TLV, #427), or kNoAckedSeqNum when the server did not report one (fall back to delay-ticks).
    // Wire ClientPrediction::reconcile() here from FlightScreen.
    static constexpr uint32_t kNoAckedSeqNum = 0xFFFFFFFFu;
    std::function<void(RenderSnapshot&, uint64_t, uint32_t, uint32_t)> snapshotCallback;

    // Optional: called when the server sends a MsgMusicState (#413/#166), with the GameState ordinal.
    // Game.cpp wires it to MusicManager::setState so a mission/AI script's world.set_music_state()
    // drives the client's music. Null = ignored. Main-thread only (onReceive runs on the main thread).
    std::function<void(uint8_t)> musicStateCallback;

    // Optional: called on a MsgHaptic (#128) with (HapticKind ordinal, a, b, durationMs). Game.cpp wires
    // it to the local gamepad via IInput. Null = ignored. Main-thread only.
    std::function<void(uint8_t, float, float, uint16_t)> hapticCallback;

    // Optional: called on a MsgRadioTransmission (#703) with (speaker, text, voiceKey, displaySeconds).
    // #704 wires it to the subtitle overlay + voice-callout pipeline. The line is always also printed to
    // the console. Null = console only. Main-thread only.
    std::function<void(const char* speaker, const char* text, const char* voiceKey, uint16_t seconds)> radioCallback;

  private:
    // Store f into *sessionFailure if it is still None (first-writer-wins via CAS); no-op if unset.
    void signalFailure(SessionFailure f);

    bool m_connected{false};
    PeerRole m_grantedRole{PeerRole::Pilot}; // role granted by MsgConnectAck (#857)
    bool m_gotConnectAck{false};             // true once a MsgConnectAck arrives; "was I admitted?" (#853)
    float m_planetRadiusKm{6371.f};
    SessionCombatStats m_sessionStats{}; // #626 — fed by CombatEvent Stats records
    fl::MissionResultCode m_missionOutcome{fl::MissionResultCode::Incomplete}; // #584 — from MsgMissionOutcome
    uint16_t m_nextReqId{1};                    // next reqId to stamp on outgoing MsgAdminCommand
    std::atomic<uint16_t> m_serverPeerCount{0}; // updated from SnapshotPeerCount TLV extension

    // Chunk reassembly state for MsgAdminResponseChunk (0x0A) streaming responses.
    std::string m_chunkBuf;
    bool m_chunkBufActive{false};
    static constexpr std::size_t kMaxChunkAssemblyBytes = 64u * 1024u; // 64 KB hard cap

    // Compressed-snapshot scratch (#775): m_decompressScratch holds the decompressed payload,
    // m_snapshotScratch the rebuilt [raw 24-byte header][payload] the unchanged parse path then
    // runs on. Both reused across packets; decompressSnapshotPayload bounds the claimed size
    // before either ever grows.
    std::vector<uint8_t> m_decompressScratch;
    std::vector<uint8_t> m_snapshotScratch;

    // Heartbeat / RTT state.
    const fl::IClock* m_clock{&fl::SystemClock::instance()};
    uint64_t m_lastSnapshotTick{0}; // highest processed WorldSnapshot tickIndex (also the server ack)
    uint32_t m_ackMask{0};          // selective-ack bitmask of recently DECODED ticks below the high-water
                                    // mark (#566); bit b = decoded tick m_lastSnapshotTick-1-b. Maintained
                                    // via fl::ackAdvance on each accepted snapshot; sent via stampAck().
    bool m_haveSnapshot{false};     // false until the first WorldSnapshot is processed (so the
                                    // legitimate first snapshot at tickIndex 0 is not dropped as stale)
    uint32_t m_lastRttMs{0};        // ms from last MsgPeerDelay; 0 = not yet received
    bool m_rttValid{false};         // true once first MsgPeerDelay with delayTicks > 0 arrives
    std::chrono::steady_clock::time_point m_lastHeartbeatSentAt{}; // throttle to 1 Hz

    // Per-peer snapshot latency (SnapshotPeerLatency TLV, ExtTag::SnapshotPeerLatency = 0x0101).
    uint16_t m_snapshotLatencyMs{0};  // ms from last snapshot TLV; 0 = not yet received
    bool m_hasSnapshotLatency{false}; // true once first non-zero SnapshotPeerLatency TLV arrives
    // Raw tick count from SnapshotPeerDelayTicks TLV (0x0102); passed to snapshotCallback for
    // client-side prediction replay depth. 0 until first non-zero TLV arrives.
    uint32_t m_estimatedDelayTicks{0};

    // #481: shared UTC clock (Julian Day) from MsgWeatherState; 0 until the first weather packet.
    double m_utcJulianDay{0.0};

    // Delta-compression entity cache: entityIdx → {gen (uint16 truncated), typeIndex, factionIndex}.
    // Populated from `full` quantized records; supplies typeIndex/factionIndex (and gen when omitted)
    // for the compact delta records that follow. Cleared implicitly when the handler is re-created per
    // session (reinitFlight).
    struct KnownEntityInfo {
        uint16_t gen;
        uint32_t typeIndex;
        uint16_t factionIndex;
    };
    std::unordered_map<uint32_t, KnownEntityInfo> m_knownEntities;

    // Faction index -> display name, from MsgFactionDef sent once after ConnectAck (#860). Used by the
    // observer entity picker to label an entity's faction.
    std::unordered_map<uint16_t, std::string> m_factionNames;

    // Entity retention cache (#516). The priority/budget scheduler omits low-priority entities from
    // some snapshots, so the rendered set must persist across packets rather than be rebuilt per
    // packet. Each entry holds the last-known render state and the tick it was last updated; entries
    // absent from a snapshot are retained until either an explicit SnapshotDespawn TLV removes them or
    // they age out past kSnapshotRetentionTicks (the backstop for interest-out / lost despawns).
    struct CachedEntity {
        fl::EntityRenderEntry re;
        uint64_t lastSeenTick{0};
    };
    std::unordered_map<uint32_t, CachedEntity> m_entityCache;

    // Datalink track picture + RWR (#528), rebuilt from each MsgDatalink. Positions are reconstructed
    // to ABSOLUTE world metres (header origin + relative payload) so the HUD needs no origin of its
    // own. Held in vectors the radarView() spans point into; replaced wholesale each message.
    std::vector<RadarTrack> m_radarTracks;
    std::vector<RwrStrobe> m_rwrStrobes;
    bool m_haveDatalink{false};

    // Crew roster + live turret pose (#972). m_crewRosters is the reliable per-entity seat roster from
    // MsgCrewRoster (keyed by entity index); m_crewTurretPoses is the per-tick mount-frame turret pose
    // decoded from each snapshot's SnapshotCrew TLV. Both main-thread only.
    std::unordered_map<uint32_t, CrewRosterInfo> m_crewRosters;
    std::unordered_map<uint32_t, std::vector<CrewTurretPose>> m_crewTurretPoses;
    SeatResultView m_lastSeatResult; // #975: the last MsgSeatResult, for the seat-picker UI
    bool m_inCrewSeat{false};        // #975: this client occupies a non-fly crew seat (a gunner)

    void handleDatalink(const void* data, std::size_t size);
    void handleCrewRoster(const void* data, std::size_t size);
    // Decode a SnapshotCrew TLV payload into m_crewTurretPoses (called from the WorldSnapshot handler).
    void applyCrewTurretTlv(const uint8_t* payload, std::size_t len);
};

} // namespace fl
