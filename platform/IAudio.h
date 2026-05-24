// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

using AudioBufferId = uint32_t;
using AudioSourceId = uint32_t;

// Threading: all methods must be called from the main thread. The OpenAL driver
// manages its own internal mixing thread; callers do not need to synchronise.
class IAudio {
  public:
    virtual ~IAudio() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Returns a human-readable description of the last error, or nullptr if none.
    // Valid until the next call on this interface.
    virtual const char* getLastError() const = 0;

    // --- Buffer management ---

    // Uploads raw PCM data and returns an opaque buffer handle, or 0 on failure.
    virtual AudioBufferId uploadBuffer(const void* data, std::size_t size, int sampleRate, int channels) = 0;
    virtual void freeBuffer(AudioBufferId id) = 0;

    // --- Source management ---

    // Creates a 3D point emitter and returns an opaque source handle, or 0 on failure.
    virtual AudioSourceId createSource() = 0;
    virtual void destroySource(AudioSourceId source) = 0;

    virtual void play(AudioSourceId source, AudioBufferId buffer) = 0;
    virtual void stop(AudioSourceId source) = 0;
    virtual void pause(AudioSourceId source) = 0;
    virtual void resume(AudioSourceId source) = 0;
    virtual bool isPlaying(AudioSourceId source) const = 0;

    virtual void setLooping(AudioSourceId source, bool loop) = 0;
    virtual void setPitch(AudioSourceId source, float pitch) = 0;
    virtual void setGain(AudioSourceId source, float gain) = 0;

    // --- 3D spatial audio ---

    virtual void setPosition(AudioSourceId source, float x, float y, float z) = 0;

    // Used by OpenAL for Doppler shift; aircraft velocity changes perceived pitch.
    virtual void setVelocity(AudioSourceId source, float vx, float vy, float vz) = 0;

    // Distance attenuation model. referenceDistance: gain = 1.0 at this distance.
    // maxDistance: gain floor (clamped beyond this). rolloffFactor: attenuation rate
    // (1.0 = linear, 0.0 = no rolloff). All three must be set per source.
    virtual void setReferenceDistance(AudioSourceId source, float dist) = 0;
    virtual void setMaxDistance(AudioSourceId source, float dist) = 0;
    virtual void setRolloffFactor(AudioSourceId source, float factor) = 0;

    // When relative=true the source position is interpreted relative to the listener rather than in
    // world space, making the source non-positional. Use for music, cockpit sounds, UI tones, and
    // radio comms. Combine with setRolloffFactor(src, 0) and setPosition(src, 0,0,0) for fully
    // head-locked audio. Defaults to false (world-space) on createSource().
    virtual void setSourceRelative(AudioSourceId source, bool relative) = 0;

    // Sets the listener (camera/player) position and orientation.
    // pos, forward, up are each three-element float arrays [x, y, z].
    virtual void setListenerTransform(const float pos[3], const float forward[3], const float up[3]) = 0;

    // Sets the listener (camera/player) velocity for Doppler calculation.
    // vel is a three-element float array [vx, vy, vz] in world units/sec.
    virtual void setListenerVelocity(const float vel[3]) = 0;
};
