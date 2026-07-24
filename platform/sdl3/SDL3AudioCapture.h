// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Concrete backend header — not a HAL interface file. Include SDL3AudioCaptureFactory.h instead.
#include "IAudioCapture.h"

#include <SDL3/SDL_audio.h>

#include <string>

namespace fl {

// SDL3 recording-device backend for IAudioCapture.
//
// SDL_AudioStream IS the ring buffer: SDL's audio thread pushes captured, format-converted frames
// into it and SDL_GetAudioStreamData drains them under SDL's own lock. So this class holds no
// buffer and no mutex of its own — adding one would be a second queue in front of a queue.
class SDL3AudioCapture final : public IAudioCapture {
  public:
    ~SDL3AudioCapture() override;

    bool init(int sampleRate, int channels, const std::string& deviceName = {}) override;
    void shutdown() override;
    const char* getLastError() const override;

    bool start() override;
    void stop() override;
    bool isCapturing() const override;

    std::size_t read(int16_t* out, std::size_t maxSamples) override;
    std::size_t available() const override;

    std::vector<std::string> listDevices() const override;
    std::string currentDevice() const override;

  private:
    SDL_AudioStream* m_stream{nullptr};
    std::string m_device; // resolved device name ("" until init succeeds)
    int m_channels{1};    // for the byte<->sample conversion in read()/available()
    bool m_capturing{false};
    bool m_ownsSubsystem{false}; // true when WE initialised SDL_INIT_AUDIO and must quit it
    mutable std::string m_lastError;
};

} // namespace fl
