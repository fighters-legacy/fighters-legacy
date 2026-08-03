// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "NullAudio.h"
#include "voice/RadioDsp.h"
#include "voice/VoiceCodec.h"
#include "voice/VoiceMixer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <vector>

using namespace fl;

namespace {

// A tracking IAudio double. mock_hal.h's MockAudio always reports zero processed buffers and
// isPlaying()==false, which is fine for one-shot SFX but would never let a STREAM recycle a buffer;
// this one models the queue so the mixer's refill loop is actually exercised.
struct StreamingAudio final : public fl::NullAudio {
    struct Src {
        std::vector<AudioBufferId> queued;
        int processed{0};
        bool playing{false};
        bool relative{false};
        float pos[3]{};
        float gain{1.f};
        float rolloff{1.f};
        float maxDist{0.f};
    };
    std::unordered_map<AudioSourceId, Src> sources;
    int buffersQueued{0};
    int sourcesCreated{0};
    int sourcesDestroyed{0};
    AudioBufferId nextBuffer{1};
    AudioSourceId nextSource{1};

    // Pretend every queued buffer has finished playing; the next update recycles them all.
    void completeAll() {
        for (auto& [id, s] : sources) {
            s.processed = static_cast<int>(s.queued.size());
        }
    }

    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        return nextBuffer++;
    }
    AudioBufferId allocStreamBuffer() override {
        return nextBuffer++;
    }
    // Record queued byte counts so a test can tell a 20 ms speech frame from a 120 ms squelch tail.
    std::vector<std::size_t> queuedBytes;
    void queueBuffer(AudioSourceId src, AudioBufferId buf, const void*, std::size_t bytes, int, int) override {
        sources[src].queued.push_back(buf);
        queuedBytes.push_back(bytes);
        ++buffersQueued;
    }
    int processedBufferCount(AudioSourceId src) override {
        return sources[src].processed;
    }
    void unqueueProcessed(AudioSourceId src, AudioBufferId* out, int maxCount) override {
        auto& s = sources[src];
        const int n = std::min(maxCount, s.processed);
        for (int i = 0; i < n; ++i) {
            out[i] = s.queued.front();
            s.queued.erase(s.queued.begin());
        }
        s.processed -= n;
    }
    void detachBuffers(AudioSourceId src) override {
        sources[src].queued.clear();
        sources[src].processed = 0;
        sources[src].playing = false;
    }
    AudioSourceId createSource() override {
        ++sourcesCreated;
        const AudioSourceId id = nextSource++;
        sources[id] = Src{};
        return id;
    }
    void destroySource(AudioSourceId src) override {
        ++sourcesDestroyed;
        sources.erase(src);
    }
    void play(AudioSourceId src, AudioBufferId) override {
        sources[src].playing = true;
    }
    void stop(AudioSourceId src) override {
        sources[src].playing = false;
    }
    void resume(AudioSourceId src) override {
        sources[src].playing = true;
    }
    bool isPlaying(AudioSourceId src) const override {
        const auto it = sources.find(src);
        return it != sources.end() && it->second.playing;
    }
    void setGain(AudioSourceId src, float g) override {
        sources[src].gain = g;
    }
    void setPosition(AudioSourceId src, float x, float y, float z) override {
        sources[src].pos[0] = x;
        sources[src].pos[1] = y;
        sources[src].pos[2] = z;
    }
    void setMaxDistance(AudioSourceId src, float d) override {
        sources[src].maxDist = d;
    }
    void setRolloffFactor(AudioSourceId src, float f) override {
        sources[src].rolloff = f;
    }
    void setSourceRelative(AudioSourceId src, bool r) override {
        sources[src].relative = r;
    }
};

// One real Opus frame, so the mixer decodes something a decoder actually accepts.
std::vector<uint8_t> realFrame(VoiceEncoder& enc) {
    std::vector<int16_t> pcm(kVoiceFrameSamples);
    for (int i = 0; i < kVoiceFrameSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kVoiceSampleRate);
        pcm[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(0.4f * 32767.f * std::sin(2.f * std::numbers::pi_v<float> * 300.f * t));
    }
    std::vector<uint8_t> out(kMaxVoicePayloadBytes);
    const std::size_t n = enc.encode(pcm, out);
    out.resize(n);
    return out;
}

VoiceSettings defaultVoiceSettings() {
    VoiceSettings v;
    v.jitterTargetFrames = 1; // no prefill delay in tests
    return v;
}

const SpeakerPositionFn kNoPositions{};

} // namespace

TEST_CASE("voice mixer tolerates a null audio device", "[voice]") {
    VoiceMixer m;
    REQUIRE_FALSE(m.init(nullptr, nullptr)); // reports "no device"...
    // ...but stays usable: headless CI and a machine with no sound card must not need an #ifdef.
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    m.onFrame(1, 0, 0, 0, std::vector<uint8_t>{1, 2, 3}, true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE_FALSE(m.anyActive());
    REQUIRE(m.duckGain() == 1.f);
}

TEST_CASE("voice mixer creates one source per speaker-net pair", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();
    REQUIRE(enc);
    const auto pkt = realFrame(*enc);

    m.onFrame(/*peer=*/7, /*entity=*/0xFFFFFFFFu, /*net=*/0, 0, pkt, true, false);
    m.onFrame(/*peer=*/7, 0xFFFFFFFFu, /*net=*/1, 0, pkt, true, false);
    m.onFrame(/*peer=*/8, 0xFFFFFFFFu, /*net=*/0, 0, pkt, true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);

    // Two nets from one speaker are two conversations, not one interleaved channel.
    REQUIRE(audio.sourcesCreated == 3);
    REQUIRE(m.activeSpeakers().size() == 3);
}

TEST_CASE("voice mixer decodes and queues audio, recycling buffers", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();
    REQUIRE(enc);

    for (uint16_t seq = 0; seq < 3; ++seq)
        m.onFrame(1, 0xFFFFFFFFu, 0, seq, realFrame(*enc), seq == 0, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(audio.buffersQueued > 0);
    const int firstPass = audio.buffersQueued;

    // Without recycling the source would starve after kStreamBuffers frames.
    audio.completeAll();
    for (uint16_t seq = 3; seq < 8; ++seq)
        m.onFrame(1, 0xFFFFFFFFu, 0, seq, realFrame(*enc), false, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(audio.buffersQueued > firstPass);
}

TEST_CASE("voice mixer head-locks a non-positional net", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    // Net 0 is the builtin TEAM net: a radio arrives in your headset.
    m.onFrame(1, /*entity=*/42, /*net=*/0, 0, realFrame(*enc), true, false);
    m.update(0.016f, glm::dvec3{}, [](uint32_t, glm::dvec3& out) {
        out = glm::dvec3{1000.0, 0.0, 0.0};
        return true;
    });
    REQUIRE(audio.sources.size() == 1);
    const auto& s = audio.sources.begin()->second;
    REQUIRE(s.relative);
    REQUIRE(s.rolloff == 0.f);
}

TEST_CASE("voice mixer places a positional net camera-relative", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    RadioNetTable nets;
    nets.add(RadioNetDef{"prox", "PROX", RadioNetKind::Proximity, /*positional=*/true, 3000.f});
    m.setNets(nets);

    // The renderer's camera-relative invariant, in audio: the source sits at world - cameraOrigin,
    // which is what keeps float32 coordinates precise at planet scale.
    const glm::dvec3 camera{6'000'000.0, 0.0, 0.0};
    m.onFrame(1, /*entity=*/42, /*net=*/0, 0, realFrame(*enc), true, false);
    m.update(0.016f, camera, [](uint32_t idx, glm::dvec3& out) {
        REQUIRE(idx == 42);
        out = glm::dvec3{6'000'250.0, 10.0, -30.0};
        return true;
    });
    const auto& s = audio.sources.begin()->second;
    REQUIRE_FALSE(s.relative);
    REQUIRE(s.pos[0] == Catch::Approx(250.f));
    REQUIRE(s.pos[1] == Catch::Approx(10.f));
    REQUIRE(s.pos[2] == Catch::Approx(-30.f));
    REQUIRE(s.maxDist == Catch::Approx(3000.f));
}

TEST_CASE("voice mixer falls back to head-locked when the speaker's entity is unknown", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    RadioNetTable nets;
    nets.add(RadioNetDef{"prox", "PROX", RadioNetKind::Proximity, true, 3000.f});
    m.setNets(nets);

    // An interest-culled speaker must not land at the world origin (which on this planet is the
    // north pole) — it must simply play centred.
    m.onFrame(1, /*entity=*/42, 0, 0, realFrame(*enc), true, false);
    m.update(0.016f, glm::dvec3{}, [](uint32_t, glm::dvec3&) { return false; });
    REQUIRE(audio.sources.begin()->second.relative);
}

TEST_CASE("voice mixer retires a stream as soon as an ended transmission drains", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    m.onFrame(1, 0xFFFFFFFFu, 0, 0, realFrame(*enc), true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(audio.sourcesDestroyed == 0);

    // The explicit end marker: no audio, just the boundary.
    m.onFrame(1, 0xFFFFFFFFu, 0, 1, std::vector<uint8_t>{}, false, true);
    audio.completeAll();
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    audio.completeAll();
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(audio.sourcesDestroyed == 1);
    REQUIRE_FALSE(m.anyActive());
}

TEST_CASE("voice mixer ignores an unknown net and an over-long payload", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    m.onFrame(1, 0xFFFFFFFFu, /*net=*/200, 0, realFrame(*enc), true, false);
    // The server's table is authoritative; a frame for a net we do not know is not ours to guess at.
    REQUIRE(audio.sourcesCreated == 0);

    std::vector<uint8_t> huge(kMaxVoicePayloadBytes + 1, 0x33);
    m.onFrame(1, 0xFFFFFFFFu, 0, 0, huge, true, false);
    REQUIRE(audio.sourcesCreated == 0);
}

TEST_CASE("voice mixer ducks while a net is live and recovers after", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    auto vs = defaultVoiceSettings();
    vs.duckingAmount = 0.5f;
    m.applySettings(vs, AudioSettings{});
    auto enc = createVoiceEncoder();

    REQUIRE(m.duckGain() == Catch::Approx(1.f));
    for (uint16_t seq = 0; seq < 6; ++seq)
        m.onFrame(1, 0xFFFFFFFFu, 0, seq, realFrame(*enc), seq == 0, false);
    for (int i = 0; i < 20; ++i)
        m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(m.duckGain() < 0.99f);

    // Recovery is gentle but must actually complete, or music never comes back after one call.
    m.reset();
    for (int i = 0; i < 120; ++i)
        m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(m.duckGain() == Catch::Approx(1.f));
}

TEST_CASE("voice mixer reset drops every stream", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();
    m.onFrame(1, 0xFFFFFFFFu, 0, 0, realFrame(*enc), true, false);
    m.onFrame(2, 0xFFFFFFFFu, 0, 0, realFrame(*enc), true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    REQUIRE(audio.sourcesCreated == 2);

    m.reset();
    REQUIRE(audio.sourcesDestroyed == 2);
    REQUIRE_FALSE(m.anyActive());
}

TEST_CASE("voice mixer brackets a transmission with the key click and the squelch tail", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();
    REQUIRE(enc);

    const std::size_t clickBytes = radioClickPcm(kVoiceSampleRate).size() * sizeof(int16_t);
    const std::size_t squelchBytes = radioSquelchPcm(kVoiceSampleRate).size() * sizeof(int16_t);
    REQUIRE(clickBytes > 0);
    REQUIRE(squelchBytes > clickBytes);

    // Net 0 is the builtin TEAM net, which carries radioEffect.
    m.onFrame(1, 0xFFFFFFFFu, 0, 0, realFrame(*enc), /*start=*/true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    // The click must come FIRST — a click after the first syllable announces nothing.
    REQUIRE_FALSE(audio.queuedBytes.empty());
    CHECK(audio.queuedBytes.front() == clickBytes);

    m.onFrame(1, 0xFFFFFFFFu, 0, 1, std::vector<uint8_t>{}, false, /*end=*/true);
    for (int i = 0; i < 4; ++i) {
        audio.completeAll();
        m.update(0.016f, glm::dvec3{}, kNoPositions);
    }
    const bool sawSquelch =
        std::find(audio.queuedBytes.begin(), audio.queuedBytes.end(), squelchBytes) != audio.queuedBytes.end();
    // The squelch is what tells a listener the transmission ENDED rather than merely paused.
    CHECK(sawSquelch);
    CHECK(audio.sourcesDestroyed == 1); // and the stream is only retired once the tail has played
}

TEST_CASE("voice mixer skips the radio cues on a net that asks for clean audio", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    m.applySettings(defaultVoiceSettings(), AudioSettings{});
    auto enc = createVoiceEncoder();

    RadioNetTable clean;
    clean.add(RadioNetDef{"intercom", "ICS", RadioNetKind::Global, /*positional=*/false, /*rangeM=*/0.f,
                          /*radioEffect=*/false});
    m.setNets(clean);

    const std::size_t clickBytes = radioClickPcm(kVoiceSampleRate).size() * sizeof(int16_t);
    m.onFrame(1, 0xFFFFFFFFu, 0, 0, realFrame(*enc), true, false);
    m.update(0.016f, glm::dvec3{}, kNoPositions);
    // "In the room" means in the room: no key click, no band-limiting, no squelch.
    REQUIRE_FALSE(audio.queuedBytes.empty());
    CHECK(audio.queuedBytes.front() != clickBytes);
}

TEST_CASE("voice mixer ducks for synthetic radio traffic via holdDuck", "[voice]") {
    StreamingAudio audio;
    VoiceMixer m;
    REQUIRE(m.init(&audio, nullptr));
    auto vs = defaultVoiceSettings();
    vs.duckingAmount = 0.5f;
    m.applySettings(vs, AudioSettings{});

    // No relayed frames at all — this is an ATC line played through VoiceCalloutManager. Without the
    // hold, the one audible difference between human and synthetic radio would be that only one of
    // them makes the music get out of the way.
    m.holdDuck(1.0f);
    for (int i = 0; i < 20; ++i)
        m.update(0.016f, glm::dvec3{}, kNoPositions);
    CHECK(m.duckGain() < 0.99f);

    for (int i = 0; i < 200; ++i)
        m.update(0.016f, glm::dvec3{}, kNoPositions);
    CHECK(m.duckGain() == Catch::Approx(1.f)); // and it lets go when the line is over
}
