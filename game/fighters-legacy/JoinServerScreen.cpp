// SPDX-License-Identifier: GPL-3.0-or-later
#include "JoinServerScreen.h"

#include "IGui.h"
#include "IInput.h"

#include <string>
#include <utility>

namespace fl {

Screen JoinServerScreen::update(IInput& input, IWindow& /*window*/, float /*frameDtS*/) {
    bool doConnect = false;
    bool doCancel = false;

    if (IGui* gui = m_deps.gui) {
        if (gui->beginWindow("Join Server", 0.32f, 0.30f, 0.36f, 0.40f)) {
            gui->label("Server address (host or host:port)");
            gui->inputText("##address", m_addr, sizeof(m_addr));
            gui->label("Join password (leave blank if none)");
            gui->inputText("##password", m_password, sizeof(m_password), /*masked=*/true);
            gui->label("Callsign");
            gui->inputText("##callsign", m_callsign, sizeof(m_callsign));
            gui->separator();
            if (gui->button("Connect"))
                doConnect = true;
            gui->sameLine();
            if (gui->button("Cancel"))
                doCancel = true;
        }
        gui->endWindow();
    }

    // Keyboard shortcuts also work with no GUI backend so the screen is never a dead end.
    if (input.isKeyJustPressed(Key::Enter))
        doConnect = true;
    if (input.isKeyJustPressed(Key::Escape))
        doCancel = true;

    if (doCancel)
        return Screen::MainMenu;
    if (doConnect)
        return confirm();
    return Screen::JoinServer;
}

Screen JoinServerScreen::confirm() {
    std::string host;
    uint16_t port = 4778;
    if (!parseConnectArg(m_addr, host, port) || host.empty())
        return Screen::JoinServer; // unusable address (empty/garbage) — stay on the form

    Result r;
    r.host = std::move(host);
    r.port = port;
    r.joinPassword = m_password;
    r.callsign = m_callsign;
    if (m_deps.onConnect)
        m_deps.onConnect(r);
    return Screen::Loading;
}

} // namespace fl
