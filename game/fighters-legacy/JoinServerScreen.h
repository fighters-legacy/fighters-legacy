// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ConnectArgs.h"
#include "IScreen.h"

#include <cstring>
#include <functional>
#include <string>

namespace fl {

class IGui;

// The multiplayer "Join Server" screen (#322, Epic E #497) — the first consumer of the IGui HAL (#156).
// It collects a server address (host[:port]), an optional join password (#998, masked), and a callsign
// (pre-filled from the pilot profile) through real text fields, then hands the connection parameters to
// the game's session layer and transitions to Loading. Cancel / Escape returns to the main menu.
//
// The screen is backend-agnostic: it drives an injected IGui and reports its result through a callback,
// so it is fully unit-testable against the scripted NullGui with no window, GPU, or network.
class JoinServerScreen : public IScreen {
  public:
    // The chosen connection, delivered to the game on Connect. `host`/`port` are already parsed from the
    // address field; `joinPassword` is empty when none was entered; `callsign` is empty to keep the
    // profile default. The game writes these into its Services connect state before entering Loading.
    struct Result {
        std::string host;
        uint16_t port{4778};
        std::string joinPassword;
        std::string callsign;
    };

    struct Deps {
        IGui* gui{nullptr};
        std::string initialHost;     // ClientSettings::lastServerHost (may be empty)
        std::string initialCallsign; // PilotProfile::callsign (may be empty)
        std::function<void(const Result&)> onConnect;
    };

    explicit JoinServerScreen(Deps deps) : m_deps(std::move(deps)) {
        setField(m_addr, sizeof(m_addr), m_deps.initialHost);
        setField(m_callsign, sizeof(m_callsign), m_deps.initialCallsign);
    }

    Screen update(IInput& input, IWindow& window, float frameDtS) override;

    // IGui screen — all rendering happens through the injected IGui in update(); no HudElements.
    std::span<const HudElement> buildElements() override {
        return {};
    }

    // Test / introspection accessors.
    [[nodiscard]] const char* address() const noexcept {
        return m_addr;
    }
    [[nodiscard]] const char* callsign() const noexcept {
        return m_callsign;
    }

  private:
    static void setField(char* buf, std::size_t cap, const std::string& v) {
        const std::size_t n = std::min(v.size(), cap - 1);
        std::memcpy(buf, v.data(), n);
        buf[n] = '\0';
    }
    // Emit the connection and hand it to the game. Returns Loading on success, JoinServer if the address
    // is unusable (empty host → stay on the screen).
    Screen confirm();

    Deps m_deps;
    char m_addr[128]{};
    char m_password[64]{};
    char m_callsign[32]{};
};

} // namespace fl
