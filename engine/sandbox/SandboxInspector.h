// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"

class IInput;
class ILogger;

class SandboxInspector {
  public:
    // freq: test tone frequency in Hz (defaults to 440 Hz / A4).
    SandboxInspector(IAudio& audio, IInput& input, ILogger& logger, float freq = 440.0f);
    ~SandboxInspector();

    // Returns false when the user requests exit (Escape key).
    // Must be called once per frame, between beginFrame() and endFrame().
    bool update();

  private:
    IAudio& m_audio;
    IInput& m_input;
    ILogger& m_logger;

    AudioBufferId m_toneBuffer{0};
    AudioSourceId m_toneSource{0};
    bool m_tonePlaying{false};
    int m_frameCount{0};
};
