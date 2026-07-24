// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3AudioCapture.h"

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cstring>

namespace fl {

SDL3AudioCapture::~SDL3AudioCapture() {
    shutdown();
}

bool SDL3AudioCapture::init(int sampleRate, int channels, const std::string& deviceName) {
    shutdown();
    m_lastError.clear();

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            m_lastError = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
            return false;
        }
        m_ownsSubsystem = true;
    }

    m_channels = std::clamp(channels, 1, 2);

    // Resolve the requested device by NAME. Ids are not stable across runs (or across a replug), so
    // the settings file stores a name; an unrecognised name falls back to the system default rather
    // than failing — a player who unplugs a headset should still be able to talk.
    SDL_AudioDeviceID devId = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!deviceName.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
        if (ids) {
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetAudioDeviceName(ids[i]);
                if (name && deviceName == name) {
                    devId = ids[i];
                    break;
                }
            }
            SDL_free(ids);
        }
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE; // the codec's format; SDL converts from whatever the device gives
    spec.channels = m_channels;
    spec.freq = sampleRate;

    // No callback: we pull with SDL_GetAudioStreamData on the main thread, so captured audio never
    // crosses a thread boundary we own.
    m_stream = SDL_OpenAudioDeviceStream(devId, &spec, nullptr, nullptr);
    if (!m_stream) {
        m_lastError = std::string("SDL_OpenAudioDeviceStream(recording) failed: ") + SDL_GetError();
        if (m_ownsSubsystem) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_ownsSubsystem = false;
        }
        return false;
    }

    const SDL_AudioDeviceID bound = SDL_GetAudioStreamDevice(m_stream);
    if (const char* name = SDL_GetAudioDeviceName(bound))
        m_device = name;
    else
        m_device = "default";
    return true;
}

void SDL3AudioCapture::shutdown() {
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    m_capturing = false;
    m_device.clear();
    if (m_ownsSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_ownsSubsystem = false;
    }
}

const char* SDL3AudioCapture::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

bool SDL3AudioCapture::start() {
    if (!m_stream)
        return false;
    if (m_capturing)
        return true;
    // Drop anything the device buffered while paused: the transmission starts NOW, not with a
    // second of whatever was said before the key went down.
    SDL_ClearAudioStream(m_stream);
    if (!SDL_ResumeAudioStreamDevice(m_stream)) {
        m_lastError = std::string("SDL_ResumeAudioStreamDevice failed: ") + SDL_GetError();
        return false;
    }
    m_capturing = true;
    return true;
}

void SDL3AudioCapture::stop() {
    if (!m_stream || !m_capturing)
        return;
    SDL_PauseAudioStreamDevice(m_stream);
    SDL_ClearAudioStream(m_stream);
    m_capturing = false;
}

bool SDL3AudioCapture::isCapturing() const {
    return m_capturing;
}

std::size_t SDL3AudioCapture::read(int16_t* out, std::size_t maxSamples) {
    if (!m_stream || !m_capturing || !out || maxSamples == 0)
        return 0;
    const int want = static_cast<int>(std::min<std::size_t>(maxSamples * sizeof(int16_t), 1u << 20));
    const int got = SDL_GetAudioStreamData(m_stream, out, want);
    if (got <= 0)
        return 0;
    return static_cast<std::size_t>(got) / sizeof(int16_t);
}

std::size_t SDL3AudioCapture::available() const {
    if (!m_stream || !m_capturing)
        return 0;
    const int bytes = SDL_GetAudioStreamAvailable(m_stream);
    return bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(int16_t) : 0u;
}

std::vector<std::string> SDL3AudioCapture::listDevices() const {
    std::vector<std::string> names;
    const bool tempInit = !SDL_WasInit(SDL_INIT_AUDIO);
    if (tempInit && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return names;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    if (ids) {
        names.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (const char* n = SDL_GetAudioDeviceName(ids[i]))
                names.emplace_back(n);
        }
        SDL_free(ids);
    }
    if (tempInit)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

std::string SDL3AudioCapture::currentDevice() const {
    return m_device;
}

} // namespace fl
