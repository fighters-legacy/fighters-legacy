// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "INetwork.h"
#include "RenderTypes.h"
#include "SessionStatus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

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

  private:
    // Store f into *sessionFailure if it is still None (first-writer-wins via CAS); no-op if unset.
    void signalFailure(SessionFailure f);

    bool m_connected{false};
};
