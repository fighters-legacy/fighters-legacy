// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builtin weapon SFX (#631): byte-stable procedural PCM, and the SfxManager voice pool +
// camera-relative positioning driven through a tracking MockAudio.
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "audio/SfxBuiltinSounds.h"
#include "audio/SfxManager.h"
#include "config/AudioSettings.h"

#include <cstdint>
#include <map>
#include <vector>

using namespace fl;

namespace {

struct NullLoggerSfx : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A MockAudio that records what SfxManager did: uploads, source creation, and the last play's
// position/gain per source.
struct TrackingAudio : IAudio {
    int uploads = 0;
    int sourcesCreated = 0;
    int plays = 0;
    AudioBufferId nextBuf = 1;
    AudioSourceId nextSrc = 1;
    struct SrcState {
        float pos[3]{};
        float gain{0.f};
    };
    std::map<AudioSourceId, SrcState> srcState;
    float listenerPos[3]{-999, -999, -999};

    bool init() override {
        return true;
    }
    void shutdown() override {}
    const char* getLastError() const override {
        return nullptr;
    }
    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        ++uploads;
        return nextBuf++;
    }
    void freeBuffer(AudioBufferId) override {}
    AudioBufferId allocStreamBuffer() override {
        return nextBuf++;
    }
    void queueBuffer(AudioSourceId, AudioBufferId, const void*, std::size_t, int, int) override {}
    int processedBufferCount(AudioSourceId) override {
        return 0;
    }
    void unqueueProcessed(AudioSourceId, AudioBufferId*, int) override {}
    void detachBuffers(AudioSourceId) override {}
    AudioSourceId createSource() override {
        ++sourcesCreated;
        return nextSrc++;
    }
    void destroySource(AudioSourceId) override {}
    void play(AudioSourceId, AudioBufferId) override {
        ++plays;
    }
    void stop(AudioSourceId) override {}
    void pause(AudioSourceId) override {}
    void resume(AudioSourceId) override {}
    bool isPlaying(AudioSourceId) const override {
        return false;
    }
    void setLooping(AudioSourceId, bool) override {}
    void setPitch(AudioSourceId, float) override {}
    void setGain(AudioSourceId s, float g) override {
        srcState[s].gain = g;
    }
    void setPosition(AudioSourceId s, float x, float y, float z) override {
        srcState[s].pos[0] = x;
        srcState[s].pos[1] = y;
        srcState[s].pos[2] = z;
    }
    void setVelocity(AudioSourceId, float, float, float) override {}
    void setSourceRelative(AudioSourceId, bool) override {}
    void setReferenceDistance(AudioSourceId, float) override {}
    void setMaxDistance(AudioSourceId, float) override {}
    void setRolloffFactor(AudioSourceId, float) override {}
    void setListenerTransform(const float pos[3], const float[3], const float[3]) override {
        listenerPos[0] = pos[0];
        listenerPos[1] = pos[1];
        listenerPos[2] = pos[2];
    }
    void setListenerVelocity(const float[3]) override {}
};

} // namespace

// ---------------------------------------------------------------------------
// Builtin sounds
// ---------------------------------------------------------------------------

TEST_CASE("generateBuiltinSfx: every kind is a valid non-empty mono buffer", "[sfx]") {
    for (SfxKind k : {SfxKind::Gunfire, SfxKind::Launch, SfxKind::Release, SfxKind::Impact, SfxKind::Explosion}) {
        const DecodedPcm p = generateBuiltinSfx(k);
        CHECK(p.valid());
        CHECK(p.channels == 1);
        CHECK(p.sampleRate == kSfxSampleRate);
        CHECK(p.samples.size() > 100u);
    }
}

TEST_CASE("generateBuiltinSfx is BYTE-STABLE across calls", "[sfx]") {
    for (SfxKind k : {SfxKind::Gunfire, SfxKind::Launch, SfxKind::Release, SfxKind::Impact, SfxKind::Explosion}) {
        const DecodedPcm a = generateBuiltinSfx(k);
        const DecodedPcm b = generateBuiltinSfx(k);
        REQUIRE(a.samples.size() == b.samples.size());
        CHECK(a.samples == b.samples); // identical PCM — a fixed hash, no rand()/time
    }
    // Different kinds are actually different.
    CHECK(generateBuiltinSfx(SfxKind::Gunfire).samples != generateBuiltinSfx(SfxKind::Explosion).samples);
}

// ---------------------------------------------------------------------------
// SfxManager
// ---------------------------------------------------------------------------

TEST_CASE("SfxManager: a null IAudio is a harmless no-op (headless / CI)", "[sfx]") {
    NullLoggerSfx log;
    SfxManager sfx;
    CHECK(sfx.init(nullptr, nullptr, &log)); // succeeds; play does nothing
    sfx.registerPreset("sfx.gunfire", "", SfxKind::Gunfire);
    const AudioSettings settings;
    sfx.play("sfx.gunfire", {0, 0, 0}, {0, 0, 0}, settings); // must not crash
    sfx.shutdown();
}

TEST_CASE("SfxManager: builtin preset uploads once and plays; unknown preset is silent", "[sfx]") {
    NullLoggerSfx log;
    TrackingAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    CHECK(audio.sourcesCreated == SfxManager::kMaxVoices);
    sfx.registerPreset("sfx.gunfire", "", SfxKind::Gunfire);

    const AudioSettings settings; // master 0.8, sfx 1.0
    sfx.play("sfx.gunfire", {100, 0, 0}, {0, 0, 0}, settings);
    sfx.play("sfx.gunfire", {200, 0, 0}, {0, 0, 0}, settings);
    CHECK(audio.plays == 2);
    CHECK(audio.uploads == 1); // decoded once, cached

    sfx.play("sfx.nope", {0, 0, 0}, {0, 0, 0}, settings); // unknown = no-op
    CHECK(audio.plays == 2);
}

TEST_CASE("SfxManager: sources are placed CAMERA-RELATIVE and gain follows the sliders", "[sfx]") {
    NullLoggerSfx log;
    TrackingAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    sfx.registerPreset("sfx.impact", "", SfxKind::Impact);

    AudioSettings settings;
    settings.masterVolume = 0.5f;
    settings.sfxVolume = 0.8f;
    // A hit at world (6.37e6 + 30, 0, 0) with the camera at (6.37e6, 0, 0): the source sits at the
    // small relative offset (30, 0, 0), NOT the planet-scale absolute — which is the whole point.
    sfx.play("sfx.impact", {6.37e6 + 30.0, 0, 0}, {6.37e6, 0, 0}, settings, /*intensity=*/2.f);

    // Find the voice that just played (the first, round-robin).
    const auto& st = audio.srcState.begin()->second;
    CHECK(st.pos[0] == 30.f);
    CHECK(st.pos[1] == 0.f);
    CHECK(st.gain == 0.5f * 0.8f * 2.f); // master × sfx × intensity
}

TEST_CASE("SfxManager: the listener sits at the origin (sources are already rebased)", "[sfx]") {
    NullLoggerSfx log;
    TrackingAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    sfx.updateListener({1, 0, 0}, {0, 1, 0});
    CHECK(audio.listenerPos[0] == 0.f);
    CHECK(audio.listenerPos[1] == 0.f);
    CHECK(audio.listenerPos[2] == 0.f);
}

TEST_CASE("SfxManager: 16 voices round-robin (steal-oldest)", "[sfx]") {
    NullLoggerSfx log;
    TrackingAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    sfx.registerPreset("sfx.gunfire", "", SfxKind::Gunfire);
    const AudioSettings settings;

    // Fire 40 rounds — every one plays, cycling through exactly kMaxVoices sources.
    for (int i = 0; i < 40; ++i)
        sfx.play("sfx.gunfire", {static_cast<double>(i), 0, 0}, {0, 0, 0}, settings);
    CHECK(audio.plays == 40);
    CHECK(static_cast<int>(audio.srcState.size()) <= SfxManager::kMaxVoices);
}

TEST_CASE("registerBuiltinSfxPresets registers the named weapon SFX presets (#869)", "[sfx]") {
    NullLoggerSfx log;
    TrackingAudio audio;
    SfxManager sfx;
    sfx.init(&audio, nullptr, &log);
    registerBuiltinSfxPresets(sfx); // the engine entry point the game client now calls

    const AudioSettings settings;
    for (const char* name : {"sfx.gunfire", "sfx.launch", "sfx.release", "sfx.impact", "sfx.explosion"})
        sfx.play(name, {0, 0, 0}, {0, 0, 0}, settings);
    CHECK(audio.plays == 5); // every named preset resolved to a builtin PCM and played

    sfx.play("sfx.nope", {0, 0, 0}, {0, 0, 0}, settings);
    CHECK(audio.plays == 5); // an unregistered name is still a silent no-op
}
