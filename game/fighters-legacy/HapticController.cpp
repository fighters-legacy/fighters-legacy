// SPDX-License-Identifier: GPL-3.0-or-later
#include "HapticController.h"

#include <algorithm>
#include <cmath>

HapticController::HapticController(IInput& input) : m_input(input) {}

void HapticController::savePrev(const fl::EntityRenderEntry* player, float agl) {
    if (player) {
        m_prevVelocity = player->velocity;
        m_prevThrottle = player->throttle;
        m_prevDamageLevel = player->damageLevel;
    }
    m_prevAgl = agl;
}

void HapticController::update(const fl::EntityRenderEntry* player, bool weaponFired, float terrainElev, float dt) {
    const bool canRumble = m_input.supportsRumble(0);
    const bool canTrigger = m_input.supportsTriggerRumble(0);

    const float agl = player ? static_cast<float>(player->position.y) - terrainElev : 0.0f;

    if (!canRumble && !canTrigger) {
        savePrev(player, agl);
        return;
    }

    // --- Gun burst ---
    if (weaponFired && canRumble)
        m_input.rumble(0, 0.0f, 0.8f, 80);

    if (player) {
        // --- Hit taken ---
        if (player->damageLevel > m_prevDamageLevel && canRumble)
            m_input.rumble(0, 0.8f, 0.4f, 120);

        // --- Stall buffet ---
        {
            glm::vec3 velBody = glm::inverse(player->orientation) * player->velocity;
            const float speed = glm::length(velBody);
            const float alpha = (speed > 1.0f) ? glm::degrees(std::atan2(-velBody.y, velBody.x)) : 0.0f;
            const bool stall = (alpha > kStallAoaDeg);
            m_stallTimer -= dt;
            if (stall && canRumble) {
                if (m_stallTimer <= 0.0f) {
                    m_input.rumble(0, 0.3f, 0.1f, 200);
                    m_stallTimer = 0.15f;
                }
            } else {
                m_stallTimer = 0.0f;
            }
        }

        // --- Afterburner ignition and sustain ---
        {
            const bool abNow = (player->throttle == 100);
            if (abNow && !m_abActive && canRumble) {
                m_input.rumble(0, 0.4f, 0.2f, 300);
                m_abTimer = 0.25f;
            }
            m_abActive = abNow;
            m_abTimer -= dt;
            if (m_abActive && m_abTimer <= 0.0f && canRumble) {
                m_input.rumble(0, 0.15f, 0.08f, 350);
                m_abTimer = 0.3f;
            }
            if (!m_abActive)
                m_abTimer = 0.0f;
        }

        // --- Engine failure (damageLevel >= 2 proxy) ---
        {
            const bool failing = (player->damageLevel >= 2);
            m_engineFailTimer -= dt;
            if (failing && canRumble) {
                if (m_engineFailTimer <= 0.0f) {
                    m_input.rumble(0, 0.5f, 0.0f, 350);
                    m_engineFailTimer = 0.3f;
                }
            } else {
                m_engineFailTimer = 0.0f;
            }
        }

        // --- G-LOC onset ---
        {
            float gLoad = 0.0f;
            if (!m_firstFrame && dt > 0.0f)
                gLoad = glm::length(player->velocity - m_prevVelocity) / (dt * 9.81f);
            const bool gloc = (gLoad > kGLocThreshold);
            m_glocTimer -= dt;
            if (gloc && canRumble) {
                if (m_glocTimer <= 0.0f) {
                    const float intensity = std::min((gLoad - kGLocThreshold) / 3.0f, 1.0f);
                    m_input.rumble(0, 0.0f, intensity * 0.6f, 200);
                    m_glocTimer = 0.15f;
                }
            } else {
                m_glocTimer = 0.0f;
            }
        }

        // --- Transonic buffet ---
        {
            const float mach = glm::length(player->velocity) / kSpeedOfSound;
            const bool transonic = (mach >= kTransonicLow && mach <= kTransonicHigh);
            m_transonicTimer -= dt;
            if (transonic && canRumble) {
                if (!m_transonicActive || m_transonicTimer <= 0.0f) {
                    m_input.rumble(0, 0.3f, 0.3f, 400);
                    m_transonicTimer = 0.5f;
                }
            } else {
                m_transonicTimer = 0.0f;
            }
            m_transonicActive = transonic;
        }

        // --- Landing gear touchdown ---
        if (agl < 2.0f && m_prevAgl >= 2.0f && canRumble)
            m_input.rumble(0, 0.9f, 0.3f, 200);

        // --- GPWS / terrain warning ---
        {
            m_gpwsTimer -= dt;
            switch (m_gpwsPhase) {
            case GpwsPhase::Idle: {
                const bool trigger = (agl < kGpwsAglThreshold && player->velocity.y < -5.0f);
                if (trigger && canRumble) {
                    m_input.rumble(0, 0.5f, 0.5f, 100);
                    m_gpwsPhase = GpwsPhase::Pulse1;
                    m_gpwsTimer = 0.1f;
                }
                break;
            }
            case GpwsPhase::Pulse1:
                if (m_gpwsTimer <= 0.0f) {
                    m_gpwsPhase = GpwsPhase::Gap;
                    m_gpwsTimer = 0.15f;
                }
                break;
            case GpwsPhase::Gap:
                if (m_gpwsTimer <= 0.0f) {
                    if (canRumble)
                        m_input.rumble(0, 0.5f, 0.5f, 100);
                    m_gpwsPhase = GpwsPhase::Pulse2;
                    m_gpwsTimer = 0.1f;
                }
                break;
            case GpwsPhase::Pulse2:
                if (m_gpwsTimer <= 0.0f) {
                    m_gpwsPhase = GpwsPhase::Cooldown;
                    m_gpwsTimer = 2.0f;
                }
                break;
            case GpwsPhase::Cooldown:
                if (m_gpwsTimer <= 0.0f)
                    m_gpwsPhase = GpwsPhase::Idle;
                break;
            }
        }
    }

    // --- Hydraulic failure (driven by notifyHydraulicFailure) ---
    {
        m_hydraulicTimer -= dt;
        if (m_hydraulicFailing && m_hydraulicInput && canRumble) {
            if (m_hydraulicTimer <= 0.0f) {
                m_input.rumble(0, 0.2f, 0.0f, 350);
                m_hydraulicTimer = 0.3f;
            }
        } else {
            m_hydraulicTimer = 0.0f;
        }
    }

    // --- Compressor stall sequence (driven by notifyCompressorStall) ---
    if (m_csPhase >= 0) {
        m_csTimer -= dt;
        if (m_csTimer <= 0.0f) {
            if (canRumble)
                m_input.rumble(0, 0.6f, 0.0f, kCsPulses[m_csPhase].durationMs);
            ++m_csPhase;
            if (m_csPhase < 4) {
                m_csTimer = kCsPulses[m_csPhase].gapBefore;
            } else {
                m_csPhase = -1;
                m_csTimer = 0.0f;
            }
        }
    }

    m_firstFrame = false;
    savePrev(player, agl);
}

void HapticController::onPause(int gamepadId) {
    m_input.stopRumble(gamepadId);
    m_stallTimer = 0.0f;
    m_abTimer = 0.0f;
    m_engineFailTimer = 0.0f;
    m_glocTimer = 0.0f;
    m_transonicTimer = 0.0f;
    m_hydraulicTimer = 0.0f;
    m_gpwsTimer = 0.0f;
    m_gpwsPhase = GpwsPhase::Idle;
    m_csPhase = -1;
    m_csTimer = 0.0f;
}

void HapticController::notifyMissileLaunch() {
    if (m_input.supportsRumble(0))
        m_input.rumble(0, 0.6f, 0.6f, 150);
}

void HapticController::notifyMissileWarning() {
    // Proper 3x50ms pulsed sequence requires a future sequencer; single burst for now.
    if (m_input.supportsRumble(0))
        m_input.rumble(0, 0.7f, 0.0f, 50);
}

void HapticController::notifyOrdnanceRelease() {
    if (m_input.supportsRumble(0))
        m_input.rumble(0, 0.4f, 0.0f, 80);
}

void HapticController::notifyCompressorStall() {
    if (m_csPhase >= 0)
        return;
    m_csPhase = 0;
    m_csTimer = kCsPulses[0].gapBefore;
}

void HapticController::notifyCarrierTrap() {
    if (m_input.supportsTriggerRumble(0))
        m_input.rumbleTriggers(0, 0.9f, 0.9f, 300);
}

void HapticController::notifyHydraulicFailure(bool active, bool anyInput) {
    m_hydraulicFailing = active;
    m_hydraulicInput = anyInput;
}
