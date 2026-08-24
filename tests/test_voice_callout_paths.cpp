// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceCalloutManager's audio-resolution paths (#1145). The existing cases in test_music_manager.cpp
// cover init/shutdown and the all-null callout; this file is the part that decides what the pilot
// actually HEARS — TTS versus a packed OGG, the radio treatment, the buffer cache, and every way
// each of those can be unavailable.
//
// The whole subsystem is built to degrade: no audio device, no assets, no synthesiser and no
// subtitles are each survivable and independent. That is only true if it is exercised.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "NullAudio.h"
#include "audio/SubtitleQueue.h"
#include "audio/VoiceCalloutManager.h"
#include "content/AssetManager.h"
#include "i18n/Localization.h"
#include "mock_content.h"
#include "mock_log.h"
#include "ogg_fixture.h"
#include "voice/RadioDsp.h"

#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

// Counts what reached the device, so a test can tell "played something" from "played nothing".
struct CountingAudio final : public NullAudio {
    int uploads{0};
    int plays{0};
    int sourcesCreated{0};
    int buffersFreed{0};
    AudioBufferId nextBuffer{1};
    AudioSourceId nextSource{1};

    AudioSourceId createSource() override {
        ++sourcesCreated;
        return nextSource++;
    }
    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        ++uploads;
        return nextBuffer++;
    }
    void play(AudioSourceId, AudioBufferId) override {
        ++plays;
    }
    void freeBuffer(AudioBufferId) override {
        ++buffersFreed;
    }
};

struct FakeAudioPack final : NullContentPack {
    std::optional<AudioBuffer> loadAudio(const char*) override {
        AudioBuffer buf;
        buf.bytes.assign(kMinimalOgg, kMinimalOgg + sizeof(kMinimalOgg));
        return buf;
    }
};

// "OggS" magic then rubbish: passes the AssetValidator gate and fails the decoder, which is the
// real shape of a corrupt pack asset.
struct BrokenOggPack final : NullContentPack {
    std::optional<AudioBuffer> loadAudio(const char*) override {
        AudioBuffer buf;
        buf.bytes = {0x4F, 0x67, 0x67, 0x53};
        buf.bytes.insert(buf.bytes.end(), 64, 0xFF);
        return buf;
    }
};

struct SilentPack final : NullContentPack {}; // has nothing at all

// A synthesiser that can succeed, fail, or return an empty (invalid) buffer.
struct FakeSynth final : public IAudioSynthesizer {
    enum class Mode { Ok, Fails, Empty } mode{Mode::Ok};
    int calls{0};
    std::string lastText;

    bool synthesise(std::string_view text, SynthesisedAudio& out) override {
        ++calls;
        lastText = text;
        if (mode == Mode::Fails)
            return false;
        if (mode == Mode::Empty) {
            out.samples.clear();
            out.sampleRate = 22050;
            out.channels = 1;
            return true; // "succeeded" with nothing in it — the valid() gate must catch this
        }
        out.samples.assign(2205, 1000); // 100 ms
        out.sampleRate = 22050;
        out.channels = 1;
        return true;
    }
};

std::unique_ptr<AssetManager> makeAssets(std::unique_ptr<IContentPack> pack, ILogger& log) {
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    auto am = std::make_unique<AssetManager>(std::move(packs), log);
    am->initialize(nullptr);
    return am;
}

AudioSettings defaultSettings() {
    AudioSettings s{};
    s.masterVolume = 1.f;
    s.voiceChatVolume = 1.f;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// init degradation
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager: init with no audio device reports it and stays usable (#1145)", "[audio][voice]") {
    NullLogger log;
    SubtitleQueue subs;
    VoiceCalloutManager vcm;
    CHECK_FALSE(vcm.init(nullptr, nullptr, &subs, nullptr, &log)); // no device: false, but not fatal

    // The subtitle path stays live — that is the documented degradation.
    vcm.playText("Fox three", nullptr, 3.f, defaultSettings());
    CHECK(subs.current() == "Fox three");
    vcm.shutdown(); // must be safe with no device
}

TEST_CASE("VoiceCalloutManager: init creates the source pool (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, nullptr, nullptr, nullptr, &log));
    CHECK(audio.sourcesCreated > 0);
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// Asset resolution and the buffer cache
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager: a packed OGG is decoded, uploaded and cached (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<FakeAudioPack>(), log);
    SubtitleQueue subs;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), &subs, nullptr, &log));

    vcm.playText("", "rwr_lock", 3.f, defaultSettings());
    CHECK(audio.uploads == 1);
    CHECK(audio.plays == 1);

    // Second call for the same asset: served from the cache, no second decode.
    vcm.playText("", "rwr_lock", 3.f, defaultSettings());
    CHECK(audio.uploads == 1);
    CHECK(audio.plays == 2);

    vcm.shutdown();
    CHECK(audio.buffersFreed >= 1); // the cache is released, not leaked
}

TEST_CASE("VoiceCalloutManager: a missing asset plays nothing and does not cache (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<SilentPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log));

    vcm.playText("", "nothing_here", 3.f, defaultSettings());
    CHECK(audio.uploads == 0);
    CHECK(audio.plays == 0);
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: an undecodable asset plays nothing (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<BrokenOggPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log));

    vcm.playText("", "corrupt", 3.f, defaultSettings());
    CHECK(audio.uploads == 0);
    CHECK(audio.plays == 0);
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// The radio treatment
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager: radio-treated audio is cached separately from the dry line (#1145)", "[audio][voice]") {
    // A radio-treated asset is a DIFFERENT buffer from the dry one. Sharing a cache key would let
    // whichever caller arrived first decide how the line sounds for everyone after it.
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<FakeAudioPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log));

    const RadioProfile profile{};
    vcm.playText("", "tower", 3.f, defaultSettings(), &profile); // treated
    const int afterTreated = audio.uploads;
    CHECK(afterTreated == 1);

    vcm.playText("", "tower", 3.f, defaultSettings()); // dry: a second, distinct buffer
    CHECK(audio.uploads == afterTreated + 1);

    vcm.playText("", "tower", 3.f, defaultSettings(), &profile); // treated again: cached
    CHECK(audio.uploads == afterTreated + 1);

    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: a missing asset under radio treatment is still silent (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<SilentPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log));

    const RadioProfile profile{};
    vcm.playText("", "nothing_here", 3.f, defaultSettings(), &profile);
    CHECK(audio.uploads == 0);
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// TTS
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager: synthesised speech wins over the packed asset (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<FakeAudioPack>(), log);
    FakeSynth synth;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log, &synth));

    vcm.playText("Bandit, left ten o'clock", "generic_callout", 3.f, defaultSettings());
    CHECK(synth.calls == 1);
    CHECK(synth.lastText == "Bandit, left ten o'clock");
    CHECK(audio.uploads == 1); // the TTS buffer, not the asset
    CHECK(audio.plays == 1);

    // TTS output is deliberately NOT cached — the same text may synthesise differently.
    vcm.playText("Bandit, left ten o'clock", "generic_callout", 3.f, defaultSettings());
    CHECK(synth.calls == 2);
    CHECK(audio.uploads == 2);
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: a failing synthesiser falls back to the packed asset (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<FakeAudioPack>(), log);
    FakeSynth synth;
    synth.mode = FakeSynth::Mode::Fails;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log, &synth));

    vcm.playText("Fox two", "rwr_lock", 3.f, defaultSettings());
    CHECK(synth.calls == 1);
    CHECK(audio.plays == 1); // the OGG covered for it
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: a synthesiser returning empty audio is not played (#1145)", "[audio][voice]") {
    // "Succeeded" with no samples: valid() is the gate, and uploading a zero-length buffer would be
    // a silent source occupying a slot in the round-robin pool.
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<SilentPack>(), log);
    FakeSynth synth;
    synth.mode = FakeSynth::Mode::Empty;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log, &synth));

    vcm.playText("Nothing comes out", nullptr, 3.f, defaultSettings());
    CHECK(synth.calls == 1);
    CHECK(audio.uploads == 0);
    CHECK(audio.plays == 0);
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: TTS under a radio profile is treated too (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    FakeSynth synth;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, nullptr, nullptr, nullptr, &log, &synth));

    const RadioProfile profile{};
    vcm.playText("Cleared hot", nullptr, 3.f, defaultSettings(), &profile);
    CHECK(synth.calls == 1);
    CHECK(audio.uploads == 1);
    CHECK(audio.plays == 1);
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// Subtitles and gain
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager: the subtitle is pushed whether or not audio played (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    SubtitleQueue subs;
    auto assets = makeAssets(std::make_unique<SilentPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), &subs, nullptr, &log));

    vcm.playText("Splash one", "missing_asset", 3.f, defaultSettings());
    CHECK(audio.plays == 0);
    CHECK(subs.current() == "Splash one"); // the pilot still reads it
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: a disabled subtitle queue swallows the text (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    SubtitleQueue subs;
    subs.setEnabled(false);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, nullptr, &subs, nullptr, &log));

    vcm.playText("Splash one", nullptr, 3.f, defaultSettings());
    CHECK(subs.current().empty());
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager: an empty asset name is not an asset request (#1145)", "[audio][voice]") {
    NullLogger log;
    CountingAudio audio;
    auto assets = makeAssets(std::make_unique<FakeAudioPack>(), log);
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, assets.get(), nullptr, nullptr, &log));

    vcm.playText("", "", 3.f, defaultSettings());
    CHECK(audio.uploads == 0);
    CHECK(audio.plays == 0);
    vcm.shutdown();
}
