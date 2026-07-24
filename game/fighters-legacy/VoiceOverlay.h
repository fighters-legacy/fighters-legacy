// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "voice/VoiceChat.h"

#include <array>
#include <cstdio>
#include <functional>
#include <span>
#include <string_view>

namespace fl {

// ---------------------------------------------------------------------------------------------
// The radio-net HUD indicator (#925)
// ---------------------------------------------------------------------------------------------
// Two things a pilot must be able to answer without leaving the fight:
//
//   "Which net am I about to transmit on?"  — a PTT key that silently sends a bandit call to ATC
//     instead of the flight is worse than one that does nothing, and the answer must be visible
//     BEFORE the key goes down, not discovered afterwards.
//   "Who is talking?"  — with a radio effect on the voice, an unfamiliar callsign is genuinely hard
//     to place, and this is also the accessibility path for a player who cannot hear the audio at
//     all (paired with the subtitle line the same transmission pushes).
//
// Drawn bottom-left, out of the way of the HUD's flight instruments and the chat/kill-feed columns.
// Owns persistent char buffers because HudElement::text is a non-owning string_view that must
// outlive endFrame (the ServerNotice / KillFeed pattern).
class VoiceOverlay {
  public:
    // Rows: the TX line plus up to three simultaneous speakers. Beyond three the net is unusable
    // anyway, and a growing list would walk up over the HUD.
    static constexpr std::size_t kMaxSpeakerRows = 3;
    static constexpr std::size_t kMaxElements = kMaxSpeakerRows + 1;

    // Resolve a speaker's participant id to a display name (the match roster).
    using NameResolver = std::function<std::string(uint32_t participantId)>;

    std::span<const HudElement> build(const VoiceChat& voice, const NameResolver& names) {
        m_count = 0;
        if (!voice.settings().enabled)
            return {};

        float y = kBaseY;
        // Local transmit state first: it is the one line that reflects what YOU are about to do.
        if (voice.transmitting()) {
            const std::string_view net = voice.netName(voice.transmittingNet());
            std::snprintf(m_buffers[m_count].data(), m_buffers[m_count].size(), "TX %.*s", static_cast<int>(net.size()),
                          net.data());
            // Level-driven brightness doubles as a "is the mic actually hearing me" check, which is
            // the first question a player asks when nobody answers.
            const float lit = 0.55f + 0.45f * std::min(1.f, voice.micLevel() * 4.f);
            emit(y, lit, lit * 0.35f, lit * 0.35f);
            y -= kRowStep;
        } else if (voice.captureAvailable() && voice.primaryNet() != kInvalidRadioNet) {
            const std::string_view net = voice.netName(voice.primaryNet());
            std::snprintf(m_buffers[m_count].data(), m_buffers[m_count].size(), "NET %.*s",
                          static_cast<int>(net.size()), net.data());
            emit(y, 0.45f, 0.55f, 0.45f);
            y -= kRowStep;
        }

        for (const ActiveSpeaker& sp : voice.mixer().activeSpeakers()) {
            if (m_count >= kMaxElements)
                break;
            const std::string name = names ? names(sp.peerId) : std::string();
            const std::string_view net = voice.netName(sp.netId);
            std::snprintf(m_buffers[m_count].data(), m_buffers[m_count].size(), "<< [%.*s] %s",
                          static_cast<int>(net.size()), net.data(), name.c_str());
            emit(y, 0.35f, 1.0f, 0.45f);
            y -= kRowStep;
        }
        return {m_elements.data(), m_count};
    }

  private:
    void emit(float y, float r, float g, float b) {
        HudElement& el = m_elements[m_count];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_buffers[m_count].data();
        el.x = kX;
        el.align = HudAlign::Left;
        el.y = y;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = 1.f;
        ++m_count;
    }

    static constexpr float kX = 0.02f;
    static constexpr float kBaseY = 0.72f;
    static constexpr float kRowStep = 0.030f;

    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::array<char, 64>, kMaxElements> m_buffers{};
    std::size_t m_count{0};
};

} // namespace fl
