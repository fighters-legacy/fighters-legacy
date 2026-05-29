// SPDX-License-Identifier: GPL-3.0-or-later
#include "sandbox/SandboxInspector.h"

#include "entity/EntityManager.h"

#include "IAudio.h"
#include "IInput.h"
#include "ILogger.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

// Fixed set of keys reported by the input monitor (covers the hardware validation use case).
static constexpr Key kMonitoredKeys[] = {
    Key::A,     Key::B,      Key::C,       Key::D,         Key::E,         Key::F,          Key::G, Key::H, Key::I,
    Key::J,     Key::K,      Key::L,       Key::M,         Key::N,         Key::O,          Key::P, Key::Q, Key::R,
    Key::S,     Key::T,      Key::U,       Key::V,         Key::W,         Key::X,          Key::Y, Key::Z, Key::Space,
    Key::Enter, Key::Escape, Key::ArrowUp, Key::ArrowDown, Key::ArrowLeft, Key::ArrowRight,
};

static constexpr const char* keyName(Key k) {
    switch (k) {
    case Key::A:
        return "A";
    case Key::B:
        return "B";
    case Key::C:
        return "C";
    case Key::D:
        return "D";
    case Key::E:
        return "E";
    case Key::F:
        return "F";
    case Key::G:
        return "G";
    case Key::H:
        return "H";
    case Key::I:
        return "I";
    case Key::J:
        return "J";
    case Key::K:
        return "K";
    case Key::L:
        return "L";
    case Key::M:
        return "M";
    case Key::N:
        return "N";
    case Key::O:
        return "O";
    case Key::P:
        return "P";
    case Key::Q:
        return "Q";
    case Key::R:
        return "R";
    case Key::S:
        return "S";
    case Key::T:
        return "T";
    case Key::U:
        return "U";
    case Key::V:
        return "V";
    case Key::W:
        return "W";
    case Key::X:
        return "X";
    case Key::Y:
        return "Y";
    case Key::Z:
        return "Z";
    case Key::Space:
        return "Space";
    case Key::Enter:
        return "Enter";
    case Key::Escape:
        return "Escape";
    case Key::ArrowUp:
        return "ArrowUp";
    case Key::ArrowDown:
        return "ArrowDown";
    case Key::ArrowLeft:
        return "ArrowLeft";
    case Key::ArrowRight:
        return "ArrowRight";
    default:
        return "Unknown";
    }
}

SandboxInspector::SandboxInspector(IAudio& audio, IInput& input, ILogger& logger, float freq,
                                   fl::EntityManager* entityManager)
    : m_audio(audio), m_input(input), m_logger(logger), m_entityManager(entityManager) {
    constexpr int kSampleRate = 44100;
    std::vector<int16_t> pcm(kSampleRate);
    for (int i = 0; i < kSampleRate; ++i)
        pcm[i] = static_cast<int16_t>(32767.0f * std::sin(2.0f * std::numbers::pi_v<float> * freq * i / kSampleRate));

    m_toneBuffer = m_audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kSampleRate, 1);
    m_toneSource = m_audio.createSource();
    m_audio.setSourceRelative(m_toneSource, true);
    m_audio.setRolloffFactor(m_toneSource, 0.0f);
    m_audio.setPosition(m_toneSource, 0.0f, 0.0f, 0.0f);
    m_audio.setLooping(m_toneSource, true);
    m_audio.setGain(m_toneSource, 0.5f);

    m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                 "sandbox inspector ready — press T to toggle audio tone, G for game master, Escape to exit");
}

SandboxInspector::~SandboxInspector() {
    m_audio.stop(m_toneSource);
    m_audio.destroySource(m_toneSource);
    m_audio.freeBuffer(m_toneBuffer);
}

bool SandboxInspector::update() {
    ++m_frameCount;

    // Entity inspector — log live count once on the first frame.
    if (m_frameCount == 1) {
        if (m_entityManager) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "entity inspector: %u live entities", m_entityManager->liveCount());
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
        } else {
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, "entity inspector: no entity manager wired");
        }
    }

    // Frame stats every 300 frames.
    if (m_frameCount % 300 == 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "sandbox: frame %d", m_frameCount);
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // Input monitor — log just-pressed keys from the monitored set.
    for (Key k : kMonitoredKeys) {
        if (m_input.isKeyJustPressed(k)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "key: %s", keyName(k));
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
        }
    }

    // Gamepad axes and buttons.
    int gpCount = m_input.getGamepadCount();
    for (int gp = 0; gp < gpCount; ++gp) {
        constexpr GamepadAxis kAxes[] = {
            GamepadAxis::LeftX,  GamepadAxis::LeftY,       GamepadAxis::RightX,
            GamepadAxis::RightY, GamepadAxis::TriggerLeft, GamepadAxis::TriggerRight,
        };
        for (GamepadAxis ax : kAxes) {
            float v = m_input.getGamepadAxis(gp, ax);
            if (std::abs(v) > 0.15f) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "gamepad %d axis %d: %.2f", gp, static_cast<int>(ax),
                              static_cast<double>(v));
                m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
            }
        }
        constexpr GamepadButton kButtons[] = {
            GamepadButton::A,
            GamepadButton::B,
            GamepadButton::X,
            GamepadButton::Y,
            GamepadButton::LeftShoulder,
            GamepadButton::RightShoulder,
            GamepadButton::Start,
            GamepadButton::Back,
        };
        for (GamepadButton btn : kButtons) {
            if (m_input.isGamepadButtonJustPressed(gp, btn)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "gamepad %d button %d", gp, static_cast<int>(btn));
                m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
            }
        }
    }

    // Game-master stub — satisfies #43 reachability requirement for Phase 1.
    if (m_input.isKeyJustPressed(Key::G))
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, "game master: not yet available (Phase 2)");

    // Audio tone toggle.
    if (m_input.isKeyJustPressed(Key::T)) {
        if (m_tonePlaying) {
            m_audio.stop(m_toneSource);
            m_tonePlaying = false;
        } else {
            m_audio.play(m_toneSource, m_toneBuffer);
            m_tonePlaying = true;
        }
    }

    // Exit.
    if (m_input.isKeyJustPressed(Key::Escape)) {
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, "sandbox inspector: exiting");
        return false;
    }

    return true;
}
