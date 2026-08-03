// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The no-op IAudio (#1117). A machine with no audio endpoint — a headless VM, an RDP session, a box
// with sound disabled in firmware, a CI runner — gets this instead of a refusal to launch. Sound is
// not a prerequisite for flying an aircraft, and every other optional subsystem in this engine
// degrades rather than aborting: a null renderer for fl-server, absent joysticks reading inert,
// missing content falling back to builtins.
//
// It is an OBJECT, not a null pointer. Consumers that take `IAudio*` already tolerate null, but the
// game also dereferences `Platform::audio` directly (the listener update every frame, the sandbox
// inspector), so "no audio" has to be something you can call. Handles are still handed out and are
// unique per instance, because callers store them, compare them against 0 for failure, and free
// them; returning 0 would read as an allocation failure and send well-written code down its error
// path on a machine that is simply quiet.
//
// Header-only and dependency-free (part of platform-hal), so tests, tools and the game share ONE
// null implementation rather than each keeping a copy that drifts as IAudio grows.
//
// Deliberately NOT `final`: test doubles derive from it and override only the handful of calls they
// actually observe, the same way `NullContentPack` and `NullNetwork` work. That is what makes a new
// `IAudio` pure virtual a one-line change here instead of an edit to every mock in tests/.

#include "IAudio.h"

namespace fl {

class NullAudio : public IAudio {
  public:
    bool init() override {
        return true;
    }
    void shutdown() override {}
    const char* getLastError() const override {
        return nullptr;
    }

    // --- Buffers ---
    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        return ++m_nextBuffer;
    }
    void freeBuffer(AudioBufferId) override {}

    AudioBufferId allocStreamBuffer() override {
        return ++m_nextBuffer;
    }
    void queueBuffer(AudioSourceId, AudioBufferId, const void*, std::size_t, int, int) override {}
    // Nothing plays, so nothing is ever ready to recycle. A streaming consumer therefore keeps its
    // slot open and simply never advances — the same shape as a track that has not finished.
    int processedBufferCount(AudioSourceId) override {
        return 0;
    }
    void unqueueProcessed(AudioSourceId, AudioBufferId*, int) override {}
    void detachBuffers(AudioSourceId) override {}

    // --- Sources ---
    AudioSourceId createSource() override {
        return ++m_nextSource;
    }
    void destroySource(AudioSourceId) override {}
    void play(AudioSourceId, AudioBufferId) override {}
    void stop(AudioSourceId) override {}
    void pause(AudioSourceId) override {}
    void resume(AudioSourceId) override {}
    bool isPlaying(AudioSourceId) const override {
        return false;
    }
    void setLooping(AudioSourceId, bool) override {}
    void setPitch(AudioSourceId, float) override {}
    void setGain(AudioSourceId, float) override {}

    // --- 3D spatial ---
    void setPosition(AudioSourceId, float, float, float) override {}
    void setVelocity(AudioSourceId, float, float, float) override {}
    void setReferenceDistance(AudioSourceId, float) override {}
    void setMaxDistance(AudioSourceId, float) override {}
    void setRolloffFactor(AudioSourceId, float) override {}
    void setSourceRelative(AudioSourceId, bool) override {}
    void setListenerTransform(const float[3], const float[3], const float[3]) override {}
    void setListenerVelocity(const float[3]) override {}

  protected:
    AudioBufferId m_nextBuffer{0};
    AudioSourceId m_nextSource{0};
};

} // namespace fl
