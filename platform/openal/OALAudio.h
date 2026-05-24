// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IAudio.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <string>
#include <unordered_map>

// Concrete backend header — not a HAL interface file.
class OALAudio : public IAudio {
  public:
    bool init() override;
    void shutdown() override;
    const char* getLastError() const override;

    // data must be 16-bit signed PCM; channels must be 1 or 2.
    AudioBufferId uploadBuffer(const void* data, std::size_t size, int sampleRate, int channels) override;
    void freeBuffer(AudioBufferId id) override;

    AudioSourceId createSource() override;
    void destroySource(AudioSourceId source) override;

    void play(AudioSourceId source, AudioBufferId buffer) override;
    void stop(AudioSourceId source) override;
    void pause(AudioSourceId source) override;
    void resume(AudioSourceId source) override;
    bool isPlaying(AudioSourceId source) const override;

    void setLooping(AudioSourceId source, bool loop) override;
    void setPitch(AudioSourceId source, float pitch) override;
    void setGain(AudioSourceId source, float gain) override;

    void setPosition(AudioSourceId source, float x, float y, float z) override;
    void setVelocity(AudioSourceId source, float vx, float vy, float vz) override;
    void setReferenceDistance(AudioSourceId source, float dist) override;
    void setMaxDistance(AudioSourceId source, float dist) override;
    void setRolloffFactor(AudioSourceId source, float factor) override;

    void setSourceRelative(AudioSourceId source, bool relative) override;

    void setListenerTransform(const float pos[3], const float forward[3], const float up[3]) override;
    void setListenerVelocity(const float vel[3]) override;

  private:
    ALCdevice* m_device{nullptr};
    ALCcontext* m_context{nullptr};

    uint32_t m_nextBufferId{1};
    uint32_t m_nextSourceId{1};
    std::unordered_map<AudioBufferId, ALuint> m_buffers;
    std::unordered_map<AudioSourceId, ALuint> m_sources;

    mutable std::string m_lastError;

    bool checkAlError(const char* ctx);
    bool checkAlcError(const char* ctx);
};
