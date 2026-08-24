// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"
#include "net/ServerBrowserModel.h" // BrowserRow

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fl {

class IGui;

// The in-game server browser (#143). An IGui table over a ServerBrowserModel's rows (LAN + lobby +
// query, merged/deduped by Game.cpp). Selecting a row — or Direct Connect — hands off to the
// JoinServerScreen prefilled; Refresh re-runs the LAN/lobby/query sweep. The screen stays free of
// sockets: Game.cpp owns the live sources and feeds it the current row snapshot each frame, so it is
// unit-testable against the scripted NullGui.
class ServerBrowserScreen : public IScreen {
  public:
    struct Deps {
        IGui* gui{nullptr};
        // The current merged rows (owned by Game.cpp; rebuilt each frame). Never null in the game, may be
        // null in a degenerate test.
        const std::vector<BrowserRow>* rows{nullptr};
        std::function<void()> onRefresh;                                    // re-sweep LAN/lobby/query
        std::function<void(const std::string& host, uint16_t port)> onJoin; // prefill JoinServerScreen
        bool lobbyEnabled{true};                                            // false => a "no lobby" status line
    };

    explicit ServerBrowserScreen(Deps deps) : m_deps(std::move(deps)) {}

    Screen update(IInput& input, IWindow& window, float frameDtS) override;
    std::span<const HudElement> buildElements() override {
        return {}; // all rendering is through IGui in update()
    }

  private:
    Deps m_deps;
};

} // namespace fl
