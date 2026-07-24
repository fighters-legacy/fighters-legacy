// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "CrewClientState.h" // client crew roster + turret pose (#972)
#include "IClock.h"
#include "INetwork.h"
#include "RenderTypes.h"
#include "SessionStatus.h"
#include "WingmanMenu.h"
#include "net/Capability.h"        // CapabilityMask / Capability — granted-authority UI gating (#949)
#include "net/GameProtocol.h"      // PeerRole, PackManifestEntry (connect handshake #853)
#include "render/RadarView.h"      // RadarView / RadarTrack / RwrStrobe (datalink picture #528)
#include "render/RenderSnapshot.h" // EntityRenderEntry (stored by value in the retention cache)
#include "voice/RadioNet.h"        // the server-authoritative radio-net table (#532)
#include "world/FactionDef.h"      // areFactionsHostile — client friend/foe fallback (#688)

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
class KillFeed;
class ChatOverlay;
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
    KillFeed* killFeed{nullptr};          // optional: multiplayer kill feed overlay, fed from the Kill branch (#647)
    ChatOverlay* chat{nullptr};           // optional: in-match chat overlay, fed from MsgChatEvent (#646)
    uint32_t motdDisplaySeconds{15};      // user-configurable; 0 = persistent

    uint32_t assignedEntityIdx{0};
    uint32_t assignedEntityGen{0};

    // Connect handshake inputs (#853). Set by Game::startGame() before net.connect(); sent as
    // MsgConnectRequest from onConnect. Defaults: pilot, server-chosen aircraft, no mounted packs.
    PeerRole requestedRole{PeerRole::Pilot};
    std::string requestedEntityType;             // empty = let the server pick its default (#834)
    std::string requestedCallsign;               // player callsign for the match roster (#996); empty = server default
    std::string requestedGuid;                   // client identity UUID for reconnect (#524); empty = don't send
    std::string requestedJoinPassword;           // join password for a private server (#998); empty = don't send
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

    // Granted authority (#949), from the MsgConnectAck ConnectAckAuthority TLV. Zero caps until a
    // granted ack arrives; re-parsed on every ConnectAck (a mid-session grant/revoke re-sends one), so
    // this reflects the current grant. UI-gating only — the server is the enforcement point.
    fl::CapabilityMask grantedCaps() const noexcept {
        return m_grantedCaps;
    }
    uint16_t grantedFactionIndex() const noexcept {
        return m_grantedFactionIndex;
    }
    // Convenience: does the client hold a given capability (e.g. to show the GM map affordance)?
    bool hasCapability(fl::Capability c) const noexcept {
        return (m_grantedCaps & fl::capBit(c)) != 0;
    }

    // Display name for a faction index (#860), from the MsgFactionDef table. Empty string if the
    // faction is unknown (no table received, or an out-of-range index).
    std::string factionName(uint16_t factionIndex) const {
        const auto it = m_factionNames.find(factionIndex);
        return it != m_factionNames.end() ? it->second : std::string{};
    }

    // ── client-side IFF (#688) ──────────────────────────────────────────────
    // This client's own faction index. The server never sends it as a dedicated field, so derive it:
    // prefer the assigned aircraft's cached faction (gen-checked), fall back to the self roster entry,
    // and return 0 (neutral) until either is known. Faction 0 has no friends and no foes.
    [[nodiscard]] uint16_t ownFactionIndex() const noexcept {
        if (assignedEntityGen != 0) {
            const auto it = m_knownEntities.find(assignedEntityIdx);
            if (it != m_knownEntities.end() && it->second.gen == static_cast<uint16_t>(assignedEntityGen))
                return it->second.factionIndex;
        }
        const auto rit = m_roster.find(m_selfPeerId);
        if (rit != m_roster.end())
            return rit->second.factionIndex;
        return 0;
    }

    // Client-side friend/foe for an ARBITRARY snapshot entity (not just datalink contacts), returning a
    // kIff* ordinal (RadarView.h). The client has no faction relationship matrix — it is Lua-mutable at
    // runtime and never crosses the wire — so this is a three-step fallback, honest about what it knows:
    //   1. same non-zero faction as ownship            -> Friend
    //   2. the entity is on a live datalink track       -> the server-computed track ident (authoritative)
    //   3. else the affiliation rule (areFactionsHostile) -> Foe / Unknown
    // Step 2 is what lets an identified contact read Friend/Foe correctly even across a coalition the
    // affiliation rule would miscall; step 3 only orders unsensed entities for the target-cycle list.
    [[nodiscard]] uint8_t identForEntity(uint32_t entityIdx, uint32_t entityGen, uint16_t factionIndex) const noexcept {
        const uint16_t own = ownFactionIndex();
        if (own != 0 && factionIndex != 0 && factionIndex == own)
            return kIffFriend;
        const auto it = m_trackIdentByEntity.find(entityIdx);
        if (it != m_trackIdentByEntity.end() && it->second.gen == static_cast<uint16_t>(entityGen))
            return it->second.ident;
        return areFactionsHostile(own, factionIndex) ? kIffFoe : kIffUnknown;
    }

    // ── match roster (#996) ─────────────────────────────────────────────────
    struct RosterEntry {
        std::string callsign;
        uint16_t factionIndex{0};
        uint8_t role{0}; // PeerRole ordinal
        bool isBot{false};
    };
    const std::unordered_map<uint32_t, RosterEntry>& roster() const noexcept {
        return m_roster;
    }
    // The single name source for chat, kill feed and scoreboard. Falls back to "Peer N" / "Bot N" when
    // a participant is not (yet) in the roster.
    std::string displayName(uint32_t participantId) const {
        const auto it = m_roster.find(participantId);
        if (it != m_roster.end() && !it->second.callsign.empty())
            return it->second.callsign;
        char buf[24];
        std::snprintf(buf, sizeof(buf), fl::isBotParticipant(participantId) ? "Bot %u" : "Peer %u", participantId);
        return buf;
    }
    // This client's own participant id, from MsgConnectAck (#996). 0 until the ack arrives — callers
    // that need to distinguish "self unknown" should check gotConnectAck().
    uint32_t selfPeerId() const noexcept {
        return m_selfPeerId;
    }

    // True after this client's own aircraft was destroyed and before it respawns (#403). Set from the
    // CombatEvent Kill branch when the victim is our entity; cleared by the next MsgConnectAck (a respawn
    // hands out a fresh entity). Drives the client's dead-pilot spectator camera.
    [[nodiscard]] bool awaitingRespawn() const noexcept {
        return m_awaitingRespawn;
    }

    // ── game-master overview map feed (#861) ─────────────────────────────────
    // The most recent complete-tick GM aggregate, reassembled from the chunked MsgGmWorldState stream
    // (reliable+ordered, so all chunks of a tick arrive contiguously and are applied in one service
    // pass). `valid` is false until the first feed arrives. Read on the main thread by GmMapOverlay.
    struct GmWorldStateView {
        bool valid{false};
        uint64_t tick{0};
        std::vector<fl::GmEntityRecord> entities;
    };
    const GmWorldStateView& gmWorldState() const noexcept {
        return m_gmWorldState;
    }

    // ── match state + scoreboard (#647/#523) ─────────────────────────────────
    // Decoded MsgMatchState: phase + limits + per-team scores. `valid` is false until the first
    // MsgMatchState arrives. Read on the main thread by the scoreboard overlay and the debrief.
    struct MatchTeam {
        uint16_t factionIndex{0};
        int32_t score{0};
    };
    struct MatchStateView {
        bool valid{false};
        uint8_t phase{0};         // MatchPhase ordinal; gate with fl::isMatchPhaseOrdinal before casting
        uint16_t scoreLimit{0};   // team score that ends the match; 0 = none
        uint64_t phaseEndTick{0}; // tick the current phase ends; 0 = untimed
        std::string modeId;
        std::string modeName;
        std::vector<MatchTeam> teamScores;
    };
    const MatchStateView& matchState() const noexcept {
        return m_matchState;
    }

    // Decoded MsgScoreboard rows, upserted by participantId across the unreliable chunked stream. Rows
    // are pruned when the matching participant leaves the roster.
    struct ScoreboardEntry {
        int32_t score{0};
        uint16_t kills{0};
        uint16_t deaths{0};
        uint16_t pingMs{0};
        uint16_t factionIndex{0};
    };
    const std::unordered_map<uint32_t, ScoreboardEntry>& scoreboard() const noexcept {
        return m_scoreboard;
    }

    // Highest processed WorldSnapshot tick — the clock the scoreboard overlay renders the match phase
    // countdown against (phaseEndTick - currentTick). 0 until the first snapshot.
    uint64_t currentTick() const noexcept {
        return m_lastSnapshotTick;
    }

    // Resolve a mission object id (e.g. "bandit1") to its network entity idx/gen from the MsgMissionRoster
    // table (#914). Returns false when the id is not in the roster (no mission, unbound player slot, or an
    // unknown id). Used by the cinematic recorder to drive entity-relative camera shots.
    bool missionEntity(const std::string& objectId, uint32_t& outIdx, uint16_t& outGen) const {
        const auto it = m_missionRoster.find(objectId);
        if (it == m_missionRoster.end())
            return false;
        outIdx = it->second.first;
        outGen = it->second.second;
        return true;
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

    // Send a chat line (#646). Reliable; the server sanitizes/rate-limits/routes it. Text is truncated to
    // kMaxChatBytes on a UTF-8 codepoint boundary. Empty text is dropped.
    void sendChat(fl::ChatChannel channel, std::string_view text);

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
    // `netId` (#925) is the radio net the line was spoken on, so synthetic traffic gets the same
    // DSP, ducking and net gain as human voice — kInvalidRadioNet = no net (a dry cockpit callout).
    std::function<void(const char* speaker, const char* text, const char* voiceKey, uint16_t seconds, uint8_t netId)>
        radioCallback;

    // ── voice comms (Epic J, #532) ──────────────────────────────────────────
    // Optional: called once per MsgVoiceNetDef with the server's radio-net table (and whether voice
    // is enabled at all). Game.cpp wires it to VoiceChat::setNets. Main-thread only.
    std::function<void(const RadioNetTable&, bool enabled)> voiceNetsCallback;

    // Optional: called per relayed voice frame. Args mirror MsgVoiceRelayHeader; `payload` is opaque
    // Opus valid only for the duration of the call. Game.cpp wires it to VoiceChat::onRemoteFrame.
    std::function<void(uint32_t senderPeerId, uint32_t senderEntityIdx, uint8_t netId, uint16_t seq,
                       std::span<const uint8_t> payload, bool start, bool end)>
        voiceFrameCallback;

    // Send one locally captured Opus frame to the server (the VoiceChat frame sink). Rides the
    // dedicated unreliable voice channel: a lost frame is 20 ms the receiver conceals, and a
    // retransmit would arrive after the moment it belonged to.
    void sendVoiceFrame(uint8_t netId, uint16_t seq, std::span<const uint8_t> payload, bool start, bool end);

  private:
    // Store f into *sessionFailure if it is still None (first-writer-wins via CAS); no-op if unset.
    void signalFailure(SessionFailure f);

    bool m_connected{false};
    PeerRole m_grantedRole{PeerRole::Pilot};            // role granted by MsgConnectAck (#857)
    fl::CapabilityMask m_grantedCaps{0};                // granted authority from the ConnectAck TLV (#949)
    uint16_t m_grantedFactionIndex{0xFFFFu};            // faction binding for a faction-scoped grant (#949)
    bool m_gotConnectAck{false};                        // true once a MsgConnectAck arrives; "was I admitted?" (#853)
    uint32_t m_selfPeerId{0};                           // this client's own participant id, from MsgConnectAck (#996)
    bool m_awaitingRespawn{false};                      // #403: own aircraft dead, awaiting respawn ack
    std::unordered_map<uint32_t, RosterEntry> m_roster; // participant id -> display record (#996)
    MatchStateView m_matchState;                        // #647/#523 — from MsgMatchState
    GmWorldStateView m_gmWorldState;                    // #861 — reassembled from MsgGmWorldState chunks
    std::unordered_map<uint32_t, ScoreboardEntry> m_scoreboard; // #647/#523 — from MsgScoreboard (upsert)
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

    // Mission object id -> {entityIdx, entityGen}, from MsgMissionRoster after ConnectAck + late deltas
    // (#914). Lets the cinematic recorder resolve an entity-relative camera shot's target to an entity.
    std::unordered_map<std::string, std::pair<uint32_t, uint16_t>> m_missionRoster;

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

    // entityIdx -> {gen, ident} for the current datalink tracks (#688). Rebuilt with each MsgDatalink
    // so identForEntity() can return the server-computed IFF for an entity that is on a live track.
    struct TrackIdent {
        uint16_t gen;
        uint8_t ident;
    };
    std::unordered_map<uint32_t, TrackIdent> m_trackIdentByEntity;

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
    // Actuator positions for remote entities (#843). Hold-last: absence means unchanged, not neutral.
    void applyArticulationTlv(const uint8_t* payload, std::size_t len);
};

} // namespace fl
