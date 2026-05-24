// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "OALAudio.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <cstdint>

static const char* alErrorStr(ALenum e) {
    switch (e) {
    case AL_NO_ERROR:
        return "AL_NO_ERROR";
    case AL_INVALID_NAME:
        return "AL_INVALID_NAME";
    case AL_INVALID_ENUM:
        return "AL_INVALID_ENUM";
    case AL_INVALID_VALUE:
        return "AL_INVALID_VALUE";
    case AL_INVALID_OPERATION:
        return "AL_INVALID_OPERATION";
    case AL_OUT_OF_MEMORY:
        return "AL_OUT_OF_MEMORY";
    default:
        return "AL_UNKNOWN_ERROR";
    }
}

static const char* alcErrorStr(ALCenum e) {
    switch (e) {
    case ALC_NO_ERROR:
        return "ALC_NO_ERROR";
    case ALC_INVALID_DEVICE:
        return "ALC_INVALID_DEVICE";
    case ALC_INVALID_CONTEXT:
        return "ALC_INVALID_CONTEXT";
    case ALC_INVALID_ENUM:
        return "ALC_INVALID_ENUM";
    case ALC_INVALID_VALUE:
        return "ALC_INVALID_VALUE";
    case ALC_OUT_OF_MEMORY:
        return "ALC_OUT_OF_MEMORY";
    default:
        return "ALC_UNKNOWN_ERROR";
    }
}

bool OALAudio::checkAlError(const char* ctx) {
    ALenum e = alGetError();
    if (e == AL_NO_ERROR)
        return true;
    m_lastError = std::string(ctx) + ": " + alErrorStr(e);
    return false;
}

bool OALAudio::checkAlcError(const char* ctx) {
    ALCenum e = alcGetError(m_device);
    if (e == ALC_NO_ERROR)
        return true;
    m_lastError = std::string(ctx) + ": " + alcErrorStr(e);
    return false;
}

const char* OALAudio::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

bool OALAudio::init() {
    m_device = alcOpenDevice(nullptr);
    if (!m_device) {
        m_lastError = "alcOpenDevice: no audio device available";
        return false;
    }
    m_context = alcCreateContext(m_device, nullptr);
    if (!m_context) {
        checkAlcError("alcCreateContext");
        alcCloseDevice(m_device);
        m_device = nullptr;
        return false;
    }
    if (!alcMakeContextCurrent(m_context)) {
        checkAlcError("alcMakeContextCurrent");
        alcDestroyContext(m_context);
        m_context = nullptr;
        alcCloseDevice(m_device);
        m_device = nullptr;
        return false;
    }
    // Flush any stale error state from OpenAL Soft's own context initialization.
    alGetError();

    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
    // Explicit Doppler config — documents that world units = metres.
    // Must be adjusted as a pair if the engine ever changes distance units.
    alDopplerFactor(1.0f);
    alSpeedOfSound(343.3f);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const float orient[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, orient);
    alListenerf(AL_GAIN, 1.0f);
    return checkAlError("init");
}

void OALAudio::shutdown() {
    for (auto& [id, al] : m_sources)
        alDeleteSources(1, &al);
    m_sources.clear();

    for (auto& [id, al] : m_buffers)
        alDeleteBuffers(1, &al);
    m_buffers.clear();

    alcMakeContextCurrent(nullptr);
    if (m_context) {
        alcDestroyContext(m_context);
        m_context = nullptr;
    }
    if (m_device) {
        alcCloseDevice(m_device);
        m_device = nullptr;
    }
}

AudioBufferId OALAudio::uploadBuffer(const void* data, std::size_t size, int sampleRate, int channels) {
    ALuint alBuf = 0;
    alGenBuffers(1, &alBuf);
    if (!checkAlError("alGenBuffers"))
        return 0;

    ALenum fmt = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    alBufferData(alBuf, fmt, data, static_cast<ALsizei>(size), static_cast<ALsizei>(sampleRate));

    if (!checkAlError("alBufferData")) {
        alDeleteBuffers(1, &alBuf);
        return 0;
    }
    AudioBufferId id = m_nextBufferId++;
    m_buffers[id] = alBuf;
    return id;
}

void OALAudio::freeBuffer(AudioBufferId id) {
    auto it = m_buffers.find(id);
    if (it == m_buffers.end())
        return;
    alDeleteBuffers(1, &it->second);
    m_buffers.erase(it);
    checkAlError("alDeleteBuffers");
}

AudioSourceId OALAudio::createSource() {
    ALuint alSrc = 0;
    alGenSources(1, &alSrc);
    if (!checkAlError("alGenSources"))
        return 0;
    AudioSourceId id = m_nextSourceId++;
    m_sources[id] = alSrc;
    return id;
}

void OALAudio::destroySource(AudioSourceId source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourceStop(it->second);
    alDeleteSources(1, &it->second);
    m_sources.erase(it);
    checkAlError("alDeleteSources");
}

void OALAudio::play(AudioSourceId source, AudioBufferId buffer) {
    auto sit = m_sources.find(source);
    auto bit = m_buffers.find(buffer);
    if (sit == m_sources.end() || bit == m_buffers.end())
        return;
    // Must stop before rebinding buffer — alSourcei(AL_BUFFER) on a playing
    // source returns AL_INVALID_OPERATION.
    alSourceStop(sit->second);
    alSourcei(sit->second, AL_BUFFER, static_cast<ALint>(bit->second));
    alSourcePlay(sit->second);
    checkAlError("play");
}

void OALAudio::stop(AudioSourceId source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourceStop(it->second);
    checkAlError("stop");
}

void OALAudio::pause(AudioSourceId source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcePause(it->second);
    checkAlError("pause");
}

void OALAudio::resume(AudioSourceId source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcePlay(it->second);
    checkAlError("resume");
}

bool OALAudio::isPlaying(AudioSourceId source) const {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return false;
    ALint state = AL_STOPPED;
    alGetSourcei(it->second, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

void OALAudio::setLooping(AudioSourceId source, bool loop) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcei(it->second, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    checkAlError("setLooping");
}

void OALAudio::setPitch(AudioSourceId source, float pitch) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcef(it->second, AL_PITCH, pitch);
    checkAlError("setPitch");
}

void OALAudio::setGain(AudioSourceId source, float gain) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcef(it->second, AL_GAIN, gain);
    checkAlError("setGain");
}

void OALAudio::setPosition(AudioSourceId source, float x, float y, float z) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSource3f(it->second, AL_POSITION, x, y, z);
    checkAlError("setPosition");
}

void OALAudio::setVelocity(AudioSourceId source, float vx, float vy, float vz) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSource3f(it->second, AL_VELOCITY, vx, vy, vz);
    checkAlError("setVelocity");
}

void OALAudio::setReferenceDistance(AudioSourceId source, float dist) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcef(it->second, AL_REFERENCE_DISTANCE, dist);
    checkAlError("setReferenceDistance");
}

void OALAudio::setMaxDistance(AudioSourceId source, float dist) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcef(it->second, AL_MAX_DISTANCE, dist);
    checkAlError("setMaxDistance");
}

void OALAudio::setRolloffFactor(AudioSourceId source, float factor) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcef(it->second, AL_ROLLOFF_FACTOR, factor);
    checkAlError("setRolloffFactor");
}

void OALAudio::setSourceRelative(AudioSourceId source, bool relative) {
    auto it = m_sources.find(source);
    if (it == m_sources.end())
        return;
    alSourcei(it->second, AL_SOURCE_RELATIVE, relative ? AL_TRUE : AL_FALSE);
    checkAlError("setSourceRelative");
}

void OALAudio::setListenerTransform(const float pos[3], const float forward[3], const float up[3]) {
    alListener3f(AL_POSITION, pos[0], pos[1], pos[2]);
    // AL_ORIENTATION: first 3 = forward, next 3 = up
    const float orient[6] = {forward[0], forward[1], forward[2], up[0], up[1], up[2]};
    alListenerfv(AL_ORIENTATION, orient);
}

void OALAudio::setListenerVelocity(const float vel[3]) {
    alListener3f(AL_VELOCITY, vel[0], vel[1], vel[2]);
    checkAlError("setListenerVelocity");
}
