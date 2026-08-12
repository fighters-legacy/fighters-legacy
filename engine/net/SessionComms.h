// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GameProtocol.h"    // CombatEventRecord, the chat/voice/scoreboard messages
#include "VoiceRouter.h"     // VoicePeerView — the per-tick recipient picture
#include "entity/EntityId.h" //
#include "voice/RadioNet.h"  // RadioNetTable — the server-authoritative net vocabulary

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class EntityManager;
class INetwork;
class WorldBroadcaster;
struct WorldBroadcasterHooks; // engine/net/WorldBroadcaster.h — the host seams (#1082, D12)

namespace atc {
struct RadioTransmission; // engine/atc/AtcTypes.h — one ATC radio line
}

// Copy an ATC RadioTransmission into its wire form; snprintf force-terminates each char[] safely.
// Free, because two callers shape the same message from different places: the ATC/voice paths here,
// and the crew-chief reply in the broadcaster's base-ops handler (#55). One copy, so a field added to
// the wire cannot reach one of them and not the other.
MsgRadioTransmission buildRadioWire(const fl::atc::RadioTransmission& tx, uint8_t netId);

// Everything the server SAYS to players that is not world state (#1087, D13).
//
// Extracted from WorldBroadcaster as a PURE relocation: text chat and its moderation veto, radio and
// ATC transmissions, the voice relay (net table, per-tick peer views, recipient selection and the
// concurrent-talker cap), the scoreboard and the kill feed, the datalink build, and the MOTD and
// server notices — with the state only they touch.
//
// A CONCRETE class owned by value, not an interface (D13) — same shape and the same
// `WorldBroadcaster&` back-reference as [[PeerAdmission]] and [[SnapshotPipeline]]; see PeerAdmission.h
// for why that seam is a friend back-reference rather than a widened public surface.
//
// The veto stays a hook, not an event (D10): `hooks.comms.chatModeration` RETURNS a decision, and an
// observer cannot refuse anything. What is emitted as an event goes to MatchEventLog instead.
//
// Sim-thread only, like the broadcaster that owns it.
class SessionComms {
  public:
    // No ILogger: nothing on this path logs. Chat, voice and scoreboard failures are answered on the
    // wire (a notice, a silent drop) rather than in the server log, so a logger member would be dead
    // weight — and clang's -Wunused-private-field is -Werror here, which is how that was caught.
    SessionComms(WorldBroadcaster& wb, EntityManager& entityManager, INetwork& net,
                 const WorldBroadcasterHooks& hooks) noexcept;

    // ── inbound (called from WorldBroadcaster::onReceive) ───────────────────
    void handleChat(uint32_t peerId, const void* data, std::size_t size);
    void handleRadioCommand(uint32_t peerId, const void* data, std::size_t size);
    void handleVoiceFrame(uint32_t peerId, const void* data, std::size_t size);

    // ── outbound ────────────────────────────────────────────────────────────
    void sendChatEvent(uint32_t peerId, uint8_t channel, uint32_t senderPeerId, std::string_view text);
    void sendNoticeTo(uint32_t peerId, const char* text);
    // The MOTD banner, unicast once on admission. A no-op when none is configured.
    void sendMotdTo(uint32_t peerId);
    // Route one ATC RadioTransmission to its recipients (#703); unset sink ⇒ logged.
    void sendRadioTransmission(const fl::atc::RadioTransmission& tx);
    // The radio-net vocabulary a client needs before it can key a mic (#532). Sent with the ConnectAck
    // burst, which is why PeerAdmission calls this.
    void sendVoiceNetDefs(uint32_t peerId);

    // ── per-tick work (called from the broadcaster's serialize phase) ───────
    void flushCombatEvents();                   // the reliable kill feed (#626)
    void broadcastDatalink(uint64_t tickIndex); // the shared team track picture (#528)
    void sendScoreboardTo(uint32_t peerId);     // one peer's full scoreboard (#523)
    void broadcastScoreboard();                 // every admitted peer's, built ONCE (#1091)

    // Queue a kill-feed record for the next flush.
    void queueCombatEvent(const CombatEventRecord& rec);
    [[nodiscard]] bool scoreboardDirty() const noexcept {
        return m_scoreboardDirty;
    }
    void markScoreboardDirty() noexcept {
        m_scoreboardDirty = true;
    }
    void clearScoreboardDirty() noexcept {
        m_scoreboardDirty = false;
    }
    // A join, leave, mute or role change alters the recipient set, so the cached per-tick views must
    // be rebuilt even within the same tick (#1090).
    void invalidateVoiceViews() noexcept {
        m_voiceViewsValid = false;
    }
    // Drop a departing peer's talker slots immediately rather than after the hold window.
    void onDisconnect(uint32_t peerId);
    // Clear match-scoped comms state for an in-process rotation.
    void resetMatchState();

    // ── configuration + admin surface (the broadcaster's public API forwards here) ──
    void setChatEnabled(bool enabled) noexcept {
        m_chatEnabled = enabled;
    }
    void setChatRateLimit(int perSecond) noexcept {
        m_chatRateLimit = perSecond < 1 ? 1 : perSecond;
    }
    bool setPeerMuted(uint32_t peerId, bool muted);
    [[nodiscard]] bool isPeerMuted(uint32_t peerId) const;
    [[nodiscard]] std::vector<uint32_t> mutedPeers() const;

    void setMotd(std::string motd);
    void setMotdDisplaySeconds(uint16_t seconds) noexcept;

    void setRadioNets(RadioNetTable nets);
    [[nodiscard]] const RadioNetTable& radioNets() const noexcept {
        return m_radioNets;
    }
    void setVoiceEnabled(bool enabled) noexcept {
        m_voiceEnabled = enabled;
    }
    [[nodiscard]] bool voiceEnabled() const noexcept {
        return m_voiceEnabled;
    }
    void setVoiceFrameRateLimit(int framesPerSecond) noexcept {
        m_voiceFrameRateLimit = framesPerSecond < 1 ? 1 : framesPerSecond;
    }
    bool setPeerVoiceMuted(uint32_t peerId, bool muted);
    [[nodiscard]] bool isPeerVoiceMuted(uint32_t peerId) const;
    [[nodiscard]] std::vector<uint32_t> voiceMutedPeers() const;
    [[nodiscard]] VoicePeerView voicePeerView(uint32_t peerId) const;
    void buildVoicePeerViews(std::vector<VoicePeerView>& out) const;

    // Additive metrics (#1090/#1091). ServerTickReport's schema version stays frozen at 6 (D18/#686).
    [[nodiscard]] uint64_t voiceRelaySends() const noexcept {
        return m_voiceRelaySends;
    }
    [[nodiscard]] uint64_t scoreboardBuilds() const noexcept {
        return m_scoreboardBuilds;
    }

  private:
    void buildScoreboardPackets(std::vector<std::vector<uint8_t>>& out);
    void appendScoreboardRows(std::vector<uint8_t>& pkt, std::size_t begin, std::size_t count,
                              const std::vector<uint32_t>& order) const;

    WorldBroadcaster& m_wb; // the match this speaks for; see the class comment
    EntityManager& m_entityManager;
    INetwork& m_net;
    const WorldBroadcasterHooks& m_hooks;

    bool m_chatEnabled{true}; // #646: false = drop all chat
    int m_chatRateLimit{2};   // #646: chat lines per second per peer

    std::string m_motd;               // empty = no MOTD sent
    uint16_t m_motdDisplaySeconds{0}; // 0 = client default

    // Kill feed (#626) + scoreboard (#523/#1091). The build counter is what a test asserts on: the
    // defect was the NUMBER of builds, so "one per window" is the property, not the bytes.
    std::vector<CombatEventRecord> m_pendingKillEvents;
    std::vector<std::vector<uint8_t>> m_scoreboardScratch;
    uint64_t m_scoreboardBuilds{0};
    bool m_scoreboardDirty{false}; // a score changed since the last broadcast

    // Voice comms (#532). The table is server-authoritative and replicated at admit time.
    RadioNetTable m_radioNets;
    bool m_voiceEnabled{true};
    // frames/s/peer. 52, not 60 (#1090): the codec produces 50 frames/s, so a 60/s limit sat ABOVE
    // the rate a well-behaved client can reach and therefore capped nothing at all. 52 leaves two
    // frames of jitter headroom and actually binds.
    int m_voiceFrameRateLimit{52};
    // Per-tick voice peer views (#1090). buildVoicePeerViews is an O(P) walk and it ran on EVERY
    // RECEIVED FRAME — at 50 frames/s per talker that is the same list rebuilt tens of thousands of
    // times a second for a picture that changes once per tick. Rebuilt at most once per tick now,
    // plus whenever membership changes within a tick (a join/leave/mute must not be missed).
    uint64_t m_voiceViewsTick{0};
    bool m_voiceViewsValid{false};
    // Per-net active-talker tracking for the concurrent-speaker cap (D20). Keyed netId -> (peerId ->
    // last frame time). A talker keeps its slot through a brief gap (kVoiceTalkerHoldMs) so a pause
    // for breath does not drop it mid-sentence and hand the slot to someone else.
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>> m_voiceTalkers;
    // Relay fan-out counter (#1090): sendChannel calls made by the voice relay. Name-keyed additive
    // metric; ServerTickReport's schema version stays frozen at 6 (D18 / #686).
    uint64_t m_voiceRelaySends{0};
    // Scratch reused by the relay path so a 50 Hz hot path does not allocate per frame.
    mutable std::vector<VoicePeerView> m_voicePeerScratch;
    mutable std::vector<uint32_t> m_voiceRecipientScratch;
    std::vector<uint8_t> m_voiceRelayScratch;
};

} // namespace fl
