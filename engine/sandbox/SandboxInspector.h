// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"

namespace fl {

class IInput;
class ILogger;
class EntityManager;

class SandboxInspector {
  public:
    // freq: test tone frequency in Hz (defaults to 440 Hz / A4).
    // entityManager: optional; when non-null, live entity count is logged each frame cycle.
    SandboxInspector(IAudio& audio, IInput& input, ILogger& logger, float freq = 440.0f,
                     EntityManager* entityManager = nullptr);
    ~SandboxInspector();

    // Returns false when the user requests exit (Escape key).
    // Must be called once per frame, between beginFrame() and endFrame().
    bool update();

  private:
    IAudio& m_audio;
    IInput& m_input;
    ILogger& m_logger;

    EntityManager* m_entityManager{nullptr};
    AudioBufferId m_toneBuffer{0};
    AudioSourceId m_toneSource{0};
    bool m_tonePlaying{false};
    int m_frameCount{0};
};

} // namespace fl
