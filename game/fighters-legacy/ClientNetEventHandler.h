// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "INetwork.h"
#include "RenderTypes.h"
#include "SessionStatus.h"
#include "render/RenderSnapshot.h" // EntityRenderEntry (stored by value in the retention cache)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace fl {

class GameConsole;
class ILogger;
class ServerNotice;
struct RenderSnapshot;
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
    EnvironmentState& env;           // updated on MsgWeatherState
    GameConsole* console{nullptr};   // optional: server notices are printed here
    ServerNotice* notice{nullptr};   // optional: server notices shown as screen banner
    uint32_t motdDisplaySeconds{15}; // user-configurable; 0 = persistent

    uint32_t assignedEntityIdx{0};
    uint32_t assignedEntityGen{0};

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

    // Active connected peer count from the last received MsgWorldSnapshot TLV extension block
    // (ExtTag::SnapshotPeerCount). Returns 0 if no extended snapshot has been received yet.
    uint16_t serverPeerCount() const noexcept {
        return m_serverPeerCount.load(std::memory_order_relaxed);
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
    // Args: (RenderSnapshot& snap, uint64_t tickIndex, uint32_t estimatedDelayTicks)
    // Wire ClientPrediction::reconcile() here from FlightScreen.
    std::function<void(RenderSnapshot&, uint64_t, uint32_t)> snapshotCallback;

  private:
    // Store f into *sessionFailure if it is still None (first-writer-wins via CAS); no-op if unset.
    void signalFailure(SessionFailure f);

    bool m_connected{false};
    float m_planetRadiusKm{6371.f};
    uint16_t m_nextReqId{1};                    // next reqId to stamp on outgoing MsgAdminCommand
    std::atomic<uint16_t> m_serverPeerCount{0}; // updated from SnapshotPeerCount TLV extension

    // Chunk reassembly state for MsgAdminResponseChunk (0x0A) streaming responses.
    std::string m_chunkBuf;
    bool m_chunkBufActive{false};
    static constexpr std::size_t kMaxChunkAssemblyBytes = 64u * 1024u; // 64 KB hard cap

    // Heartbeat / RTT state.
    const fl::IClock* m_clock{&fl::SystemClock::instance()};
    uint64_t m_lastSnapshotTick{0}; // highest processed WorldSnapshot tickIndex (also the server ack)
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

    // Delta-compression entity cache: entityIdx → {gen (uint16 truncated), typeIndex}.
    // Populated from `full` quantized records; supplies typeIndex (and gen when omitted) for the
    // compact delta records that follow. Cleared implicitly when the handler is re-created per
    // session (reinitFlight).
    struct KnownEntityInfo {
        uint16_t gen;
        uint32_t typeIndex;
    };
    std::unordered_map<uint32_t, KnownEntityInfo> m_knownEntities;

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
};

} // namespace fl
