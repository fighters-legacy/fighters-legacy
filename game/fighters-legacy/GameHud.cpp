// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameHud.h"

#include "render/CameraController.h"
#include "render/RenderSnapshot.h"

#include <cstring>

void GameHud::setNotice(std::string_view text, uint16_t secondsRemaining) {
    std::snprintf(m_noticeBuf, sizeof(m_noticeBuf), "%.*s", static_cast<int>(text.size()), text.data());
    m_noticeSecsLeft = secondsRemaining;
    m_hasNotice = true;
}

void GameHud::update(fl::CameraMode mode, const fl::EntityRenderEntry* player, float timeOfDay) {
    // Flight HUD is only meaningful in Cockpit mode.
    const fl::EntityRenderEntry* hudEntry = (mode == fl::CameraMode::Cockpit) ? player : nullptr;
    m_flightHud.update(hudEntry, timeOfDay);
}

std::span<const HudElement> GameHud::buildElements() {
    m_elements.clear();

    auto flightElems = m_flightHud.elements();
    m_elements.insert(m_elements.end(), flightElems.begin(), flightElems.end());

    if (m_hasNotice) {
        HudElement el;
        el.type = HudElement::Type::Text;
        el.x = 0.5f;
        el.y = 0.02f;
        el.r = 1.f;
        el.g = 0.9f;
        el.b = 0.1f;
        el.a = 1.f;
        el.scale = 1.f;
        el.text = m_noticeBuf;
        m_elements.push_back(el);
    }

    return m_elements;
}
