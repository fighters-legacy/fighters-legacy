// SPDX-License-Identifier: GPL-3.0-or-later
//
// Continuous engine + aerodynamic sound layers (#959). Two halves: the pure throttle/airspeed →
// pitch/gain mapping + byte-stable builtin loops (engine-audio), and the EngineAudioManager source
// wiring (own-ship head-locked, positional flyby doppler) driven headless through a tracking IAudio.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EngineAudioManager.h"
#include "ILogger.h"
#include "NullAudio.h"
#include "audio/EngineAudio.h"
#include "config/AudioSettings.h"

#include <map>
#include <vector>

using namespace fl;
using Catch::Approx;

namespace {

struct NullLoggerEa : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records the per-source state EngineAudioManager sets, so the doppler / head-lock / distance wiring
// can be asserted without a device.
struct TrackingAudio : fl::NullAudio {
    struct SrcState {
        bool playing{false};
        bool looping{false};
        bool relative{false};
        float gain{-1.f};
        float pitch{-1.f};
        float refDist{-1.f};
        float maxDist{-1.f};
        float rolloff{-1.f};
        float pos[3]{999.f, 999.f, 999.f};
        float vel[3]{999.f, 999.f, 999.f};
        int playCount{0};
        int stopCount{0};
    };
    std::map<AudioSourceId, SrcState> src;
    int uploads{0};
    AudioBufferId nextBuf{1};
    AudioSourceId nextSrc{1};

    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        ++uploads;
        return nextBuf++;
    }
    AudioSourceId createSource() override {
        AudioSourceId s = nextSrc++;
        src[s];
        return s;
    }
    void destroySource(AudioSourceId s) override {
        src.erase(s);
    }
    void play(AudioSourceId s, AudioBufferId) override {
        src[s].playing = true;
        ++src[s].playCount;
    }
    void stop(AudioSourceId s) override {
        src[s].playing = false;
        ++src[s].stopCount;
    }
    bool isPlaying(AudioSourceId s) const override {
        auto it = src.find(s);
        return it != src.end() && it->second.playing;
    }
    void setLooping(AudioSourceId s, bool l) override {
        src[s].looping = l;
    }
    void setPitch(AudioSourceId s, float p) override {
        src[s].pitch = p;
    }
    void setGain(AudioSourceId s, float g) override {
        src[s].gain = g;
    }
    void setPosition(AudioSourceId s, float x, float y, float z) override {
        src[s].pos[0] = x;
        src[s].pos[1] = y;
        src[s].pos[2] = z;
    }
    void setVelocity(AudioSourceId s, float x, float y, float z) override {
        src[s].vel[0] = x;
        src[s].vel[1] = y;
        src[s].vel[2] = z;
    }
    void setReferenceDistance(AudioSourceId s, float d) override {
        src[s].refDist = d;
    }
    void setMaxDistance(AudioSourceId s, float d) override {
        src[s].maxDist = d;
    }
    void setRolloffFactor(AudioSourceId s, float f) override {
        src[s].rolloff = f;
    }
    void setSourceRelative(AudioSourceId s, bool r) override {
        src[s].relative = r;
    }
};

EntityRenderEntry makeEntry(uint32_t idx, glm::dvec3 pos, glm::vec3 vel, uint8_t throttle, bool ab) {
    EntityRenderEntry e{};
    e.entityIdx = idx;
    e.entityGen = 1;
    e.typeIndex = 0;
    e.position = pos;
    e.velocity = vel;
    e.throttle = throttle;
    e.abEngaged = ab;
    return e;
}

} // namespace

TEST_CASE("engineTone maps throttle to pitch and gain") {
    const EngineToneParams idle = engineTone(0.f, 0.f, false);
    const EngineToneParams mil = engineTone(1.f, 0.f, false);
    CHECK(idle.gain == Approx(kEngineIdleGain));
    CHECK(mil.gain == Approx(kEngineMilGain));
    CHECK(idle.pitch == Approx(kEngineBasePitch));
    CHECK(mil.pitch > idle.pitch); // more throttle = higher pitch
}

TEST_CASE("engineTone afterburner adds a distinct pitch and gain jump") {
    const EngineToneParams dry = engineTone(1.f, 0.f, false);
    const EngineToneParams wet = engineTone(1.f, 0.f, true);
    CHECK(wet.gain == Approx(dry.gain + kEngineAbGainBoost));
    CHECK(wet.pitch > dry.pitch);
}

TEST_CASE("engineTone raises pitch with airspeed (ram) and clamps to a safe range") {
    CHECK(engineTone(0.5f, 340.f, false).pitch > engineTone(0.5f, 0.f, false).pitch);
    // A pathological input can never drive AL_PITCH outside the safe window.
    const EngineToneParams hot = engineTone(1.f, 100000.f, true);
    CHECK(hot.pitch <= kEngineMaxPitch);
    CHECK(hot.pitch >= kEngineMinPitch);
}

TEST_CASE("windRushGain scales with dynamic pressure and clamps") {
    CHECK(windRushGain(0.f) == Approx(0.f));
    CHECK(windRushGain(kWindFullMps) == Approx(kWindMaxGain));
    // ∝ airspeed²: half the full speed is a quarter of the gain.
    CHECK(windRushGain(0.5f * kWindFullMps) == Approx(0.25f * kWindMaxGain));
    // Beyond the full-gain speed it saturates rather than growing without bound.
    CHECK(windRushGain(10.f * kWindFullMps) == Approx(kWindMaxGain));
}

TEST_CASE("windRushPitch creeps up with airspeed from the base") {
    CHECK(windRushPitch(0.f) == Approx(kWindBasePitch));
    CHECK(windRushPitch(200.f) > windRushPitch(0.f));
    CHECK(windRushPitch(100000.f) <= kEngineMaxPitch);
}

TEST_CASE("builtin engine loops are non-empty and byte-stable") {
    for (EngineLoopKind k : {EngineLoopKind::Engine, EngineLoopKind::Wind}) {
        const DecodedPcm a = generateEngineLoopPcm(k);
        const DecodedPcm b = generateEngineLoopPcm(k);
        CHECK(!a.samples.empty());
        CHECK(a.channels == 1);
        CHECK(a.sampleRate == kEngineAudioSampleRate);
        CHECK(a.samples == b.samples); // deterministic: a golden test can pin it
    }
}

TEST_CASE("EngineAudioManager: own engine is head-locked, flyby is positional with doppler") {
    TrackingAudio audio;
    NullLoggerEa log;
    EngineAudioManager mgr;
    REQUIRE(mgr.init(&audio, &log));
    // No air predicate ⇒ every non-own entity qualifies.

    std::vector<EntityRenderEntry> ents;
    ents.push_back(makeEntry(1, {0, 0, 0}, {100, 0, 0}, 80, true));     // ownship, AB lit
    ents.push_back(makeEntry(2, {500, 0, 0}, {0, 0, -200}, 60, false)); // a flyby, closing fast

    AudioSettings s;
    s.masterVolume = 0.5f;
    s.sfxVolume = 0.5f;
    mgr.update(ents, /*ownIdx=*/1, /*cameraOrigin=*/{0, 0, 0}, s);

    REQUIRE(mgr.activeFlybyVoices() == 1);

    // The head-locked own-engine source is the relative one whose gain follows the ENGINE curve (the
    // own-WIND layer is also relative + rolloff 0, so the two are told apart by their gain). The
    // flyby is the world-space (non-relative) playing source with a distance model.
    const EngineToneParams expect = engineTone(0.80f, 100.f, true);
    const TrackingAudio::SrcState* own = nullptr;
    const TrackingAudio::SrcState* flyby = nullptr;
    for (const auto& [id, st] : audio.src) {
        if (st.relative && st.rolloff == Approx(0.f) && st.playing && st.gain == Approx(expect.gain * 0.25f))
            own = &st;
        else if (!st.relative && st.playing && st.refDist > 0.f)
            flyby = &st;
    }
    REQUIRE(own != nullptr);
    REQUIRE(flyby != nullptr);

    // Own engine: velocity pinned to the ownship (no doppler on your own jet), looping.
    CHECK(own->vel[0] == Approx(100.f));
    CHECK(own->looping);

    // Flyby: positioned camera-relative (source at 500,0,0 since camera at origin), the entity's world
    // velocity set so OpenAL applies doppler, distance model configured.
    CHECK(flyby->pos[0] == Approx(500.f));
    CHECK(flyby->vel[2] == Approx(-200.f));
    CHECK(flyby->refDist == Approx(EngineAudioManager::kFlybyReferenceDistanceM));
    CHECK(flyby->looping);
}

TEST_CASE("EngineAudioManager: a flyby leaving audible range stops its voice") {
    TrackingAudio audio;
    NullLoggerEa log;
    EngineAudioManager mgr;
    mgr.init(&audio, &log);

    std::vector<EntityRenderEntry> ents;
    ents.push_back(makeEntry(1, {0, 0, 0}, {0, 0, 0}, 50, false));
    ents.push_back(makeEntry(2, {300, 0, 0}, {0, 0, 0}, 50, false));
    mgr.update(ents, 1, {0, 0, 0}, AudioSettings{});
    REQUIRE(mgr.activeFlybyVoices() == 1);

    // Push the flyby well beyond the audible ceiling.
    ents[1].position = {static_cast<double>(EngineAudioManager::kFlybyMaxDistanceM) * 2.0, 0, 0};
    mgr.update(ents, 1, {0, 0, 0}, AudioSettings{});
    CHECK(mgr.activeFlybyVoices() == 0);
}

TEST_CASE("EngineAudioManager: the air-vehicle predicate excludes non-aircraft") {
    TrackingAudio audio;
    NullLoggerEa log;
    EngineAudioManager mgr;
    mgr.init(&audio, &log);
    mgr.setAirVehiclePredicate([](uint32_t typeIndex) { return typeIndex == 7; }); // only type 7 is "air"

    std::vector<EntityRenderEntry> ents;
    ents.push_back(makeEntry(1, {0, 0, 0}, {0, 0, 0}, 50, false)); // own (type 0)
    EntityRenderEntry ground = makeEntry(2, {200, 0, 0}, {0, 0, 0}, 90, false);
    ground.typeIndex = 3; // not air
    ents.push_back(ground);
    mgr.update(ents, 1, {0, 0, 0}, AudioSettings{});
    CHECK(mgr.activeFlybyVoices() == 0); // the ground vehicle gets no engine layer
}

TEST_CASE("EngineAudioManager tolerates a null audio device") {
    NullLoggerEa log;
    EngineAudioManager mgr;
    REQUIRE(mgr.init(nullptr, &log));
    std::vector<EntityRenderEntry> ents;
    ents.push_back(makeEntry(1, {0, 0, 0}, {50, 0, 0}, 50, false));
    mgr.update(ents, 1, {0, 0, 0}, AudioSettings{}); // no crash
    CHECK(mgr.activeFlybyVoices() == 0);
}
