// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerBrowserScreen.h"

#include "IGui.h"
#include "IInput.h"
#include "IWindow.h"

#include <cstdio>
#include <string>

namespace fl {

namespace {
// Compose a compact one-line label for a browser row (the selectable list idiom).
std::string rowLabel(const BrowserRow& r) {
    char buf[256];
    const char* status = r.protocolMismatch ? " [incompatible]" : (r.shuttingDown ? " [closing]" : "");
    char ping[24];
    if (r.hasPing)
        std::snprintf(ping, sizeof(ping), "%.0f ms", static_cast<double>(r.pingMs));
    else
        std::snprintf(ping, sizeof(ping), "-- ms");
    std::snprintf(buf, sizeof(buf), "%s  |  %s  |  %d/%d  |  %s  |  %s%s",
                  r.name.empty() ? r.host.c_str() : r.name.c_str(), r.mode.empty() ? "-" : r.mode.c_str(), r.players,
                  r.maxPlayers, ping, r.passworded ? "locked" : "open", status);
    return buf;
}
} // namespace

Screen ServerBrowserScreen::update(IInput& input, IWindow& /*window*/) {
    Screen next = Screen::ServerBrowser;
    bool goDirect = false;

    if (IGui* gui = m_deps.gui) {
        if (gui->beginWindow("Server Browser", 0.15f, 0.12f, 0.70f, 0.72f)) {
            if (gui->button("Refresh") && m_deps.onRefresh)
                m_deps.onRefresh();
            gui->sameLine();
            if (gui->button("Direct Connect"))
                goDirect = true;
            gui->sameLine();
            if (gui->button("Cancel"))
                next = Screen::MainMenu;
            gui->separator();

            if (!m_deps.lobbyEnabled)
                gui->label("Internet lobby disabled (no HTTP backend) — showing LAN servers only.");

            const std::vector<BrowserRow>* rows = m_deps.rows;
            if (!rows || rows->empty()) {
                gui->label("No servers found. Refresh, or use Direct Connect.");
            } else {
                gui->label("Server  |  Mode  |  Players  |  Ping  |  Access");
                for (const BrowserRow& r : *rows) {
                    if (gui->selectable(rowLabel(r), false)) {
                        if (m_deps.onJoin)
                            m_deps.onJoin(r.host, r.gamePort);
                        next = Screen::JoinServer; // hand off to the prefilled connect form
                    }
                }
            }
        }
        gui->endWindow();
    }

    if (goDirect)
        return Screen::JoinServer;
    // Keyboard fallbacks (work when no GUI backend is present).
    if (input.isKeyJustPressed(Key::Escape))
        return Screen::MainMenu;
    if (input.isKeyJustPressed(Key::Enter))
        return Screen::JoinServer;
    return next;
}

} // namespace fl
