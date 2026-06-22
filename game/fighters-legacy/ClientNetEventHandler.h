// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "INetwork.h"
#include "RenderTypes.h"
#include "SessionStatus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

class GameConsole;
class ILogger;
class ServerNotice;
class INetwork;

namespace fl {
class EntityTypeRegistry;
class SimRenderBridge;
} // namespace fl

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
    fl::SimRenderBridge& bridge;
    fl::EntityTypeRegistry& registry;
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

    ClientNetEventHandler(fl::SimRenderBridge& b, fl::EntityTypeRegistry& r, ILogger& l, INetwork& n,
                          EnvironmentState& e)
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
    uint64_t m_lastSnapshotTick{0}; // last received WorldSnapshot tickIndex
    uint32_t m_lastRttMs{0};        // ms from last MsgPeerDelay; 0 = not yet received
    bool m_rttValid{false};         // true once first MsgPeerDelay with delayTicks > 0 arrives
    std::chrono::steady_clock::time_point m_lastHeartbeatSentAt{}; // throttle to 1 Hz

    // Delta-compression entity cache: entityIdx → {gen (uint16 truncated), typeIndex}.
    // Populated from full MsgEntityEntry records; used to decode compact MsgEntityUpdate records.
    // Cleared implicitly when ClientNetEventHandler is re-created per session (reinitFlight).
    struct KnownEntityInfo {
        uint16_t gen;
        uint32_t typeIndex;
    };
    std::unordered_map<uint32_t, KnownEntityInfo> m_knownEntities;
};
