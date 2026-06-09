// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IInput.h"
#include "render/RenderSnapshot.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class HapticController {
  public:
    explicit HapticController(IInput& input);

    // Called every frame. terrainElev is used to derive AGL from player position.
    void update(const fl::EntityRenderEntry* player, bool weaponFired, float terrainElev, float dt);

    // Call on pause, console open, window focus loss, and game exit.
    void onPause(int gamepadId = 0);

    // Entry points for future game systems — not yet wired to in-engine events.
    void notifyMissileLaunch();
    void notifyMissileWarning();
    void notifyOrdnanceRelease();
    void notifyCompressorStall();
    void notifyCarrierTrap();
    void notifyHydraulicFailure(bool active, bool anyInput);

  private:
    static constexpr float kStallAoaDeg = 18.0f;
    static constexpr float kGLocThreshold = 6.0f;
    static constexpr float kGpwsAglThreshold = 300.0f;
    static constexpr float kTransonicLow = 0.85f;
    static constexpr float kTransonicHigh = 1.05f;
    static constexpr float kSpeedOfSound = 340.3f;

    enum class GpwsPhase { Idle, Pulse1, Gap, Pulse2, Cooldown };

    struct CsPulse {
        float gapBefore;
        uint32_t durationMs;
    };
    static constexpr CsPulse kCsPulses[4] = {{0.04f, 30}, {0.065f, 30}, {0.05f, 30}, {0.08f, 30}};

    IInput& m_input;

    bool m_firstFrame{true};
    glm::vec3 m_prevVelocity{};
    uint8_t m_prevThrottle{0};
    uint8_t m_prevDamageLevel{0};
    float m_prevAgl{0.0f};

    bool m_abActive{false};
    bool m_transonicActive{false};
    bool m_hydraulicFailing{false};
    bool m_hydraulicInput{false};

    float m_stallTimer{0.0f};
    float m_abTimer{0.0f};
    float m_engineFailTimer{0.0f};
    float m_glocTimer{0.0f};
    float m_transonicTimer{0.0f};
    float m_hydraulicTimer{0.0f};
    float m_gpwsTimer{0.0f};

    GpwsPhase m_gpwsPhase{GpwsPhase::Idle};

    int m_csPhase{-1};
    float m_csTimer{0.0f};

    void savePrev(const fl::EntityRenderEntry* player, float agl);
};
