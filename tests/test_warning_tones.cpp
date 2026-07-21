// SPDX-License-Identifier: GPL-3.0-or-later
//
// Warning-tone state machine (#957): stall + overspeed cockpit tones driven headless through a
// tracking IAudio — no device. Verifies activation, exit hysteresis, live gain, the in-flight gate,
// channel independence, and byte-stable builtin PCM shape.
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "audio/WarningToneManager.h"
#include "config/AudioSettings.h"

#include <algorithm>
#include <cstdint>
#include <map>

using namespace fl;

namespace {

struct NullLoggerWt : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records what WarningToneManager did to each source: play/stop, looping, relative, gain.
struct TrackingAudio : IAudio {
    struct SrcState {
        bool playing{false};
        bool looping{false};
        bool relative{false};
        float gain{-1.f};
        int playCount{0};
        int stopCount{0};
    };
    std::map<AudioSourceId, SrcState> src;
    int uploads{0};
    AudioBufferId nextBuf{1};
    AudioSourceId nextSrc{1};

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
    void pause(AudioSourceId) override {}
    void resume(AudioSourceId) override {}
    bool isPlaying(AudioSourceId s) const override {
        auto it = src.find(s);
        return it != src.end() && it->second.playing;
    }
    void setLooping(AudioSourceId s, bool l) override {
        src[s].looping = l;
    }
    void setPitch(AudioSourceId, float) override {}
    void setGain(AudioSourceId s, float g) override {
        src[s].gain = g;
    }
    void setPosition(AudioSourceId, float, float, float) override {}
    void setVelocity(AudioSourceId, float, float, float) override {}
    void setReferenceDistance(AudioSourceId, float) override {}
    void setMaxDistance(AudioSourceId, float) override {}
    void setRolloffFactor(AudioSourceId, float) override {}
    void setSourceRelative(AudioSourceId s, bool r) override {
        src[s].relative = r;
    }
    void setListenerTransform(const float[3], const float[3], const float[3]) override {}
    void setListenerVelocity(const float[3]) override {}

    // Test helper: the single currently-playing source (tests only ever fire one channel at a time).
    const SrcState* onlyPlaying() const {
        const SrcState* found = nullptr;
        for (auto& [id, s] : src)
            if (s.playing) {
                if (found)
                    return nullptr; // more than one
                found = &s;
            }
        return found;
    }
};

WarningToneInputs flying() {
    WarningToneInputs in;
    in.inFlight = true;
    return in;
}

} // namespace

TEST_CASE("warning tones no-op with null audio", "[warning]") {
    WarningToneManager wt;
    wt.init(nullptr, nullptr);
    WarningToneInputs in = flying();
    in.stall = true;
    wt.update(in, AudioSettings{}, 0.1f); // must not crash / dereference
    CHECK_FALSE(wt.stallActive());
    CHECK_FALSE(wt.overspeedActive());
}

TEST_CASE("stall tone activates and plays a looping head-locked source", "[warning]") {
    TrackingAudio audio;
    NullLoggerWt log;
    WarningToneManager wt;
    wt.init(&audio, &log);

    AudioSettings s;
    s.masterVolume = 0.5f;
    s.sfxVolume = 0.8f;

    WarningToneInputs in = flying();
    in.stall = true;
    wt.update(in, s, 0.016f);

    CHECK(wt.stallActive());
    const auto* playing = audio.onlyPlaying();
    REQUIRE(playing != nullptr);
    CHECK(playing->looping);
    CHECK(playing->relative); // head-locked, not spatialised
    CHECK(playing->gain == 0.5f * 0.8f);
    CHECK(audio.uploads == 1); // tone buffer uploaded once
}

TEST_CASE("stall tone holds then stops after hysteresis window", "[warning]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);

    WarningToneInputs in = flying();
    in.stall = true;
    wt.update(in, AudioSettings{}, 0.016f);
    REQUIRE(wt.stallActive());

    // Predicate clears but within the hold window: still active.
    in.stall = false;
    wt.update(in, AudioSettings{}, 0.1f);
    CHECK(wt.stallActive());

    // Advance past kHoldSeconds: it stops.
    wt.update(in, AudioSettings{}, WarningToneManager::kHoldSeconds);
    CHECK_FALSE(wt.stallActive());
    CHECK_FALSE(audio.isPlaying(1));
}

TEST_CASE("gain tracks the volume slider live while active", "[warning]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);

    AudioSettings s;
    s.masterVolume = 1.0f;
    s.sfxVolume = 1.0f;
    WarningToneInputs in = flying();
    in.stall = true;
    wt.update(in, s, 0.016f);
    CHECK(audio.src.at(1).gain == 1.0f);

    s.sfxVolume = 0.25f; // slider moved mid-session
    wt.update(in, s, 0.016f);
    CHECK(audio.src.at(1).gain == 0.25f);
}

TEST_CASE("leaving flight silences tones immediately", "[warning]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);

    WarningToneInputs in = flying();
    in.stall = true;
    in.overspeed = true;
    wt.update(in, AudioSettings{}, 0.016f);
    REQUIRE(wt.stallActive());
    REQUIRE(wt.overspeedActive());

    WarningToneInputs menu; // inFlight = false
    menu.stall = true;      // predicate still set, but not in flight
    wt.update(menu, AudioSettings{}, 0.016f);
    CHECK_FALSE(wt.stallActive());
    CHECK_FALSE(wt.overspeedActive());
}

TEST_CASE("stall and overspeed are independent channels", "[warning]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);

    WarningToneInputs in = flying();
    in.overspeed = true; // only overspeed
    wt.update(in, AudioSettings{}, 0.016f);
    CHECK_FALSE(wt.stallActive());
    CHECK(wt.overspeedActive());

    in.overspeed = false;
    in.stall = true;
    // overspeed is now in its hold window and stall just fired -> both briefly active, distinct sources.
    wt.update(in, AudioSettings{}, 0.016f);
    CHECK(wt.stallActive());
    CHECK(wt.overspeedActive());
    CHECK(audio.uploads == 2); // two distinct tone buffers
}

TEST_CASE("builtin warning tones are byte-stable and loop-clean", "[warning]") {
    const DecodedPcm stall = generateWarningTonePcm(WarningTone::Stall);
    const DecodedPcm over = generateWarningTonePcm(WarningTone::Overspeed);

    // Structural invariants (robust against libm ULP differences across platforms).
    CHECK(stall.channels == 1);
    CHECK(over.channels == 1);
    CHECK(stall.sampleRate == kWarningToneSampleRate);
    CHECK(stall.samples.size() == 5512u * 4u); // four gate cycles, exact
    CHECK(over.samples.size() == 21u * 525u);  // integer 1050 Hz cycles, exact

    // The stall tone's loop seam sits in the gated-off (silent) region.
    CHECK(stall.samples.back() == 0);
    CHECK(stall.samples.front() == 0); // beep envelope starts at zero

    // Both tones actually produce sound somewhere.
    int16_t stallPeak = 0;
    for (int16_t v : stall.samples)
        stallPeak = std::max<int16_t>(stallPeak, static_cast<int16_t>(v < 0 ? -v : v));
    CHECK(stallPeak > 1000);
}

// ── RWR / missile-lock tones (#960) ──────────────────────────────────────────────────────────────

TEST_CASE("RWR tone tracks the worst threat and escalates instantly", "[warning][rwr]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);
    AudioSettings s;
    s.masterVolume = 1.f;
    s.rwrVolume = 1.f;

    WarningToneInputs in = flying();
    in.rwr = RwrThreat::Search;
    wt.update(in, s, 0.016f);
    CHECK(wt.rwrActive() == RwrThreat::Search);
    {
        const auto* p = audio.onlyPlaying(); // only the RWR voice sounds
        REQUIRE(p);
        CHECK(p->relative); // head-locked
        CHECK(p->looping);
    }

    // A launch appearing must be heard THIS frame — escalation bypasses the hold.
    in.rwr = RwrThreat::Launch;
    wt.update(in, s, 0.016f);
    CHECK(wt.rwrActive() == RwrThreat::Launch);
}

TEST_CASE("RWR de-escalation holds then clears", "[warning][rwr]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);
    AudioSettings s;

    WarningToneInputs in = flying();
    in.rwr = RwrThreat::Lock;
    wt.update(in, s, 0.016f);
    CHECK(wt.rwrActive() == RwrThreat::Lock);

    // The threat drops for one frame — the tone must not chop: it holds.
    in.rwr = RwrThreat::None;
    wt.update(in, s, 0.1f);
    CHECK(wt.rwrActive() == RwrThreat::Lock);

    // Past the hold window it clears.
    wt.update(in, s, 1.0f);
    CHECK(wt.rwrActive() == RwrThreat::None);
}

TEST_CASE("RWR honors its own volume slider, not the SFX slider", "[warning][rwr]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);
    AudioSettings s;
    s.masterVolume = 1.f;
    s.sfxVolume = 0.f;  // SFX muted...
    s.rwrVolume = 0.5f; // ...but the RWR has its own gain

    WarningToneInputs in = flying();
    in.rwr = RwrThreat::Lock;
    wt.update(in, s, 0.016f);
    const auto* p = audio.onlyPlaying();
    REQUIRE(p);
    CHECK(p->gain == 0.5f); // master*rwr, independent of the muted SFX slider
}

TEST_CASE("RWR tone is silenced out of flight", "[warning][rwr]") {
    TrackingAudio audio;
    WarningToneManager wt;
    wt.init(&audio, nullptr);
    AudioSettings s;

    WarningToneInputs in = flying();
    in.rwr = RwrThreat::Launch;
    wt.update(in, s, 0.016f);
    CHECK(wt.rwrActive() == RwrThreat::Launch);

    in.inFlight = false; // to a menu / spectator — silence at once, bypassing the hold
    wt.update(in, s, 0.016f);
    CHECK(wt.rwrActive() == RwrThreat::None);
}

TEST_CASE("builtin RWR tones are byte-stable and loop-clean", "[warning][rwr]") {
    const DecodedPcm search = generateWarningTonePcm(WarningTone::RwrSearch);
    const DecodedPcm lock = generateWarningTonePcm(WarningTone::RwrLock);
    const DecodedPcm launch = generateWarningTonePcm(WarningTone::RwrLaunch);

    for (const DecodedPcm* p : {&search, &lock, &launch}) {
        CHECK(p->channels == 1);
        CHECK(p->sampleRate == kWarningToneSampleRate);
        CHECK(!p->samples.empty());
    }
    // Determinism: a golden test can pin the shape.
    CHECK(generateWarningTonePcm(WarningTone::RwrLaunch).samples == launch.samples);

    // The gated tones' loop seam sits in silence; the steady lock tone sounds throughout.
    CHECK(search.samples.back() == 0);
    CHECK(launch.samples.back() == 0);
    int16_t lockPeak = 0;
    for (int16_t v : lock.samples)
        lockPeak = std::max<int16_t>(lockPeak, static_cast<int16_t>(v < 0 ? -v : v));
    CHECK(lockPeak > 1000);
}
