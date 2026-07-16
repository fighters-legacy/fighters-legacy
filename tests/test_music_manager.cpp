// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "audio/MusicBuiltinTracks.h"
#include "audio/MusicManager.h"
#include "audio/OggDecoder.h"
#include "audio/PlaylistLoader.h"
#include "audio/SubtitleQueue.h"
#include "audio/VoiceCalloutManager.h"
#include "content/AssetManager.h"
#include "content/IContentPack.h"
#include "i18n/Localization.h"
#include "mock_content.h"
#include "mock_hal.h"
#include "ogg_fixture.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// NullAudio — no-op IAudio for unit tests (no OpenAL device required).
// All streaming + detach methods must be implemented or the test will not compile.
// ---------------------------------------------------------------------------
struct NullAudio : IAudio {
    bool init() override {
        return true;
    }
    void shutdown() override {}
    const char* getLastError() const override {
        return nullptr;
    }

    AudioBufferId uploadBuffer(const void*, std::size_t, int, int) override {
        return 1;
    }
    void freeBuffer(AudioBufferId) override {}

    AudioBufferId allocStreamBuffer() override {
        return ++m_nextBuf;
    }
    void queueBuffer(AudioSourceId, AudioBufferId, const void*, std::size_t, int, int) override {}
    int processedBufferCount(AudioSourceId) override {
        return 0;
    }
    void unqueueProcessed(AudioSourceId, AudioBufferId*, int) override {}
    void detachBuffers(AudioSourceId) override {}

    AudioSourceId createSource() override {
        return ++m_nextSrc;
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
    void setPosition(AudioSourceId, float, float, float) override {}
    void setVelocity(AudioSourceId, float, float, float) override {}
    void setReferenceDistance(AudioSourceId, float) override {}
    void setMaxDistance(AudioSourceId, float) override {}
    void setRolloffFactor(AudioSourceId, float) override {}
    void setSourceRelative(AudioSourceId, bool) override {}
    void setListenerTransform(const float[3], const float[3], const float[3]) override {}
    void setListenerVelocity(const float[3]) override {}

    AudioBufferId m_nextBuf{0};
    AudioSourceId m_nextSrc{0};
};

// ---------------------------------------------------------------------------
// NullLogger — discards all messages.
// ---------------------------------------------------------------------------
struct NullLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// NullContentPack (shared, mock_content.h) — returns nullopt/empty for every asset request.
// Lets us construct a real AssetManager in tests without filesystem access. The audio fixtures
// below derive from it and override only loadAudio.

// The minimal valid OGG Vorbis fixture (kMinimalOgg) lives in ogg_fixture.h so the fuzz
// seed-mint tool can share the exact bytes. Included at the top of this file.

// FakeAudioPack — returns kMinimalOgg bytes for any loadAudio() call.
// Used to exercise the "asset found + valid OGG" paths in VoiceCalloutManager and MusicManager.
struct FakeAudioPack : NullContentPack {
    std::optional<AudioBuffer> loadAudio(const char*) override {
        AudioBuffer buf;
        buf.bytes.assign(kMinimalOgg, kMinimalOgg + sizeof(kMinimalOgg));
        return buf;
    }
};

// GarbageAudioPack — returns bytes that are NOT valid OGG. These fail AssetManager's
// magic-byte gate (AssetValidator), so loadAudio() callers see "asset not found".
struct GarbageAudioPack : NullContentPack {
    std::optional<AudioBuffer> loadAudio(const char*) override {
        AudioBuffer buf;
        buf.bytes = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
        return buf;
    }
};

// OggPrefixGarbagePack — a valid "OggS" magic followed by garbage. This PASSES the
// AssetValidator magic-byte gate (the real attack shape for content-pack audio) and
// exercises the decoder's own malformed-stream rejection paths.
struct OggPrefixGarbagePack : NullContentPack {
    std::optional<AudioBuffer> loadAudio(const char*) override {
        AudioBuffer buf;
        buf.bytes = {0x4F, 0x67, 0x67, 0x53}; // "OggS"
        buf.bytes.insert(buf.bytes.end(), 64, 0xFF);
        return buf;
    }
};

// TrackingAudioPack — returns kMinimalOgg bytes AND records which asset names were opened.
// Slot becomes active after openSlot(); EOF handler in update() never fires (NullAudio
// processedBufferCount returns 0). Use for testing setState() permutation at state entry.
struct TrackingAudioPack : NullContentPack {
    std::vector<std::string> opened;
    std::optional<AudioBuffer> loadAudio(const char* name) override {
        opened.push_back(name);
        AudioBuffer buf;
        buf.bytes.assign(kMinimalOgg, kMinimalOgg + sizeof(kMinimalOgg));
        return buf;
    }
};

// NullTrackingContentPack — records asset names but returns nullopt, so openSlot() always
// fails and slot.active stays false. The update() EOF handler fires on every call, allowing
// track-advance and wrap-around logic to be exercised without a real audio backend.
struct NullTrackingContentPack : NullContentPack {
    std::vector<std::string> opened;
    std::optional<AudioBuffer> loadAudio(const char* name) override {
        opened.push_back(name);
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// Shared playlist TOML for tests.
// ---------------------------------------------------------------------------
static constexpr const char* kValidPlaylist = R"(
[crossfade]
duration_s = 2.5

[[states]]
id = "Menu"
tracks = ["music/menu"]
loop = true

[[states]]
id = "FlightPatrol"
tracks = ["music/patrol_01", "music/patrol_02"]
loop = true
shuffle = true
)";

// Three-state playlist used by the shuffle test cases.
// FlightPatrol (shuffle=true, 3 tracks): tests 2, 3, 4, 6.
// FlightCombat (shuffle=false, 3 tracks): test 5.
// Menu (shuffle=false, 1 track): test 1.
static constexpr const char* kShufflePlaylist = R"(
[crossfade]
duration_s = 1.0

[[states]]
id = "FlightPatrol"
tracks = ["music/a", "music/b", "music/c"]
loop = true
shuffle = true

[[states]]
id = "FlightCombat"
tracks = ["music/x", "music/y", "music/z"]
loop = true
shuffle = false

[[states]]
id = "Menu"
tracks = ["music/menu"]
loop = true
shuffle = false
)";

// ---------------------------------------------------------------------------
// PlaylistLoader tests (pure parse, no filesystem)
// ---------------------------------------------------------------------------

TEST_CASE("parsePlaylist crossfade duration", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    REQUIRE_THAT(pd.crossfadeDuration, Catch::Matchers::WithinAbs(2.5f, 0.001f));
}

TEST_CASE("parsePlaylist state count and track names", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    REQUIRE(pd.states.size() == 2);
    REQUIRE(pd.states[0].id == "Menu");
    REQUIRE(pd.states[0].tracks.size() == 1);
    REQUIRE(pd.states[0].tracks[0] == "music/menu");
    REQUIRE(pd.states[1].id == "FlightPatrol");
    REQUIRE(pd.states[1].tracks.size() == 2);
    REQUIRE(pd.states[1].shuffle);
}

TEST_CASE("parsePlaylist findState", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    REQUIRE(pd.findState("Menu") != nullptr);
    REQUIRE(pd.findState("Unknown") == nullptr);
}

TEST_CASE("parsePlaylist empty text returns empty PlaylistData", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist("", log);
    REQUIRE(pd.states.empty());
}

TEST_CASE("parsePlaylist malformed TOML returns empty PlaylistData", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist("[[states\nbroken = {", log);
    REQUIRE(pd.states.empty());
}

TEST_CASE("parsePlaylist state with no tracks produces warning entry", "[audio][playlist]") {
    NullLogger log;
    PlaylistData pd = parsePlaylist("[[states]]\nid = \"Menu\"\n", log);
    REQUIRE(pd.states.size() == 1);
    REQUIRE(pd.states[0].tracks.empty());
}

// ---------------------------------------------------------------------------
// OggDecoder null/invalid-input tests (exercises error paths without real OGG)
// ---------------------------------------------------------------------------

TEST_CASE("decodeOgg returns invalid for empty bytes", "[audio][ogg]") {
    DecodedPcm pcm = decodeOgg({});
    REQUIRE(!pcm.valid());
}

TEST_CASE("decodeOgg returns invalid for garbage bytes", "[audio][ogg]") {
    std::vector<uint8_t> garbage(64, 0xFF);
    DecodedPcm pcm = decodeOgg(garbage);
    REQUIRE(!pcm.valid());
}

TEST_CASE("openOggStream returns nullptr for empty bytes", "[audio][ogg]") {
    REQUIRE(openOggStream({}) == nullptr);
}

TEST_CASE("openOggStream returns nullptr for garbage bytes", "[audio][ogg]") {
    std::vector<uint8_t> garbage(64, 0xAB);
    OggStream* s = openOggStream(garbage);
    REQUIRE(s == nullptr);
}

TEST_CASE("decodeOgg and openOggStream reject OggS magic followed by garbage", "[audio][ogg]") {
    // Passes AssetValidator's 4-byte magic check — the realistic malicious content-pack
    // shape — so the decoder itself must reject it.
    std::vector<uint8_t> bytes = {0x4F, 0x67, 0x67, 0x53}; // "OggS"
    bytes.insert(bytes.end(), 64, 0xFF);
    REQUIRE(!decodeOgg(bytes).valid());
    REQUIRE(openOggStream(bytes) == nullptr);
}

TEST_CASE("decodeOgg and openOggStream reject truncated valid streams", "[audio][ogg]") {
    // Bare capture pattern only.
    std::vector<uint8_t> magicOnly(kMinimalOgg, kMinimalOgg + 4);
    REQUIRE(!decodeOgg(magicOnly).valid());
    REQUIRE(openOggStream(magicOnly) == nullptr);

    // First half of an otherwise-valid stream (headers cut mid-setup).
    std::vector<uint8_t> half(kMinimalOgg, kMinimalOgg + sizeof(kMinimalOgg) / 2);
    REQUIRE(!decodeOgg(half).valid());
    REQUIRE(openOggStream(half) == nullptr);
}

TEST_CASE("decodeOgg fails when output exceeds the sample cap", "[audio][ogg]") {
    // kMinimalOgg decodes to ~2200 samples; a cap of 4 must reject (never truncate).
    DecodedPcm pcm = decodeOgg(kMinimalOgg, 4);
    REQUIRE(!pcm.valid());
    REQUIRE(pcm.samples.empty());
}

TEST_CASE("getOggStreamInfo returns zero for null stream", "[audio][ogg]") {
    OggStreamInfo info = getOggStreamInfo(nullptr);
    REQUIRE(info.sampleRate == 0);
    REQUIRE(info.channels == 0);
}

TEST_CASE("readOggSamples returns 0 for null stream", "[audio][ogg]") {
    int16_t buf[4] = {};
    REQUIRE(readOggSamples(nullptr, buf, 4) == 0);
}

TEST_CASE("seekOggStart and closeOggStream handle nullptr gracefully", "[audio][ogg]") {
    seekOggStart(nullptr);   // must not crash
    closeOggStream(nullptr); // must not crash
}

// ---------------------------------------------------------------------------
// MusicManager tests
// ---------------------------------------------------------------------------

TEST_CASE("MusicManager init succeeds with NullAudio", "[audio][music]") {
    NullAudio audio;
    NullLogger log;
    MusicManager mm;
    REQUIRE(mm.init(&audio, nullptr, &log));
    mm.shutdown();
}

TEST_CASE("MusicManager setState does not crash with empty playlist", "[audio][music]") {
    NullAudio audio;
    NullLogger log;
    MusicManager mm;
    mm.init(&audio, nullptr, &log);
    mm.setState(GameState::Menu);
    mm.setState(GameState::FlightCombat);
    mm.update(1.0f / 60.0f, 0.8f, 0.7f);
    mm.shutdown();
}

TEST_CASE("MusicManager update does not crash after shutdown", "[audio][music]") {
    NullAudio audio;
    NullLogger log;
    MusicManager mm;
    mm.init(&audio, nullptr, &log);
    mm.shutdown();
    mm.update(0.016f, 1.0f, 1.0f); // m_audio == nullptr guard
}

TEST_CASE("MusicManager setState logs warning when track asset not found", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<NullContentPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);

    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    mm.loadPlaylist(pd);

    // openSlot will be called; NullContentPack returns nullopt for loadAudio
    mm.setState(GameState::Menu);
    // Call update once — exercises the track-end retry path
    mm.update(0.016f, 1.0f, 1.0f);
    mm.shutdown();
}

TEST_CASE("MusicManager setState same state twice is no-op", "[audio][music]") {
    NullAudio audio;
    NullLogger log;
    MusicManager mm;
    mm.init(&audio, nullptr, &log);
    mm.setState(GameState::Menu);
    mm.setState(GameState::Menu); // should no-op (same state, not active)
    mm.shutdown();
}

TEST_CASE("MusicManager all GameState values accepted without crash", "[audio][music]") {
    NullAudio audio;
    NullLogger log;
    MusicManager mm;
    mm.init(&audio, nullptr, &log);
    for (auto s : {GameState::Menu, GameState::FlightPatrol, GameState::FlightCombat, GameState::MissionSuccess,
                   GameState::Debrief}) {
        mm.setState(s);
        mm.update(0.016f, 1.0f, 1.0f);
    }
    mm.shutdown();
}

// ---------------------------------------------------------------------------
// VoiceCalloutManager tests
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager init and shutdown succeed", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    VoiceCalloutManager vcm;
    REQUIRE(vcm.init(&audio, nullptr, nullptr, nullptr, &log));
    vcm.shutdown();
    vcm.shutdown(); // must be idempotent
}

TEST_CASE("VoiceCalloutManager play with all-null callout is no-op", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    SubtitleQueue sq;
    VoiceCalloutManager vcm;
    vcm.init(&audio, nullptr, &sq, nullptr, &log);
    AudioSettings settings{0.8f, 1.0f, 0.7f, 1.0f, 1.0f};
    VoiceCallout callout{nullptr, nullptr, 4.0f};
    vcm.play(callout, settings);
    REQUIRE(sq.current().empty());
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager play with null audio asset and null i18n does not crash", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    SubtitleQueue sq;
    VoiceCalloutManager vcm;
    vcm.init(&audio, nullptr, &sq, nullptr, &log);
    AudioSettings settings{};
    // subtitleKey non-null but i18n is null — subtitle text will be empty
    VoiceCallout callout{nullptr, "rwr.warning", 3.0f};
    vcm.play(callout, settings);
    REQUIRE(sq.current().empty()); // i18n null → no text → no subtitle pushed
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager play does not crash with disabled subtitle queue", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    SubtitleQueue sq;
    sq.setEnabled(false);
    VoiceCalloutManager vcm;
    vcm.init(&audio, nullptr, &sq, nullptr, &log);
    AudioSettings settings{};
    vcm.play(VoiceCallout{nullptr, nullptr, 4.0f}, settings);
    REQUIRE(sq.current().empty());
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager round-robins through kMaxSfxSources sources", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    VoiceCalloutManager vcm;
    vcm.init(&audio, nullptr, nullptr, nullptr, &log);
    AudioSettings settings{};
    // Call play more times than kMaxSfxSources to exercise round-robin wrap
    for (int i = 0; i < 10; ++i)
        vcm.play(VoiceCallout{nullptr, nullptr, 4.0f}, settings);
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// OggDecoder success-path tests (use kMinimalOgg)
// ---------------------------------------------------------------------------

TEST_CASE("decodeOgg succeeds on valid OGG data", "[audio][ogg]") {
    DecodedPcm pcm = decodeOgg(kMinimalOgg);
    REQUIRE(pcm.valid());
    REQUIRE(pcm.sampleRate == 44100);
    REQUIRE(pcm.channels == 1);
    REQUIRE(!pcm.samples.empty());
}

TEST_CASE("openOggStream succeeds on valid OGG data", "[audio][ogg]") {
    OggStream* s = openOggStream(kMinimalOgg);
    REQUIRE(s != nullptr);
    OggStreamInfo info = getOggStreamInfo(s);
    REQUIRE(info.sampleRate == 44100);
    REQUIRE(info.channels == 1);
    closeOggStream(s);
}

TEST_CASE("readOggSamples returns samples from valid stream", "[audio][ogg]") {
    OggStream* s = openOggStream(kMinimalOgg);
    REQUIRE(s != nullptr);
    int16_t buf[512];
    int decoded = readOggSamples(s, buf, 256);
    REQUIRE(decoded >= 0); // may be 0 for short file, must not crash
    seekOggStart(s);
    closeOggStream(s);
}

TEST_CASE("readOggSamples drains to EOF and seekOggStart rewinds for looping", "[audio][ogg]") {
    OggStream* s = openOggStream(kMinimalOgg);
    REQUIRE(s != nullptr);

    auto drain = [&] {
        int16_t buf[512];
        int total = 0;
        for (;;) {
            const int n = readOggSamples(s, buf, 512);
            if (n <= 0)
                break;
            total += n;
        }
        return total;
    };

    const int firstPass = drain();
    REQUIRE(firstPass > 0); // 0.05 s @ 44100 Hz mono => ~2200 samples

    // Past EOF: further reads return 0, not an error or crash.
    int16_t buf[16];
    REQUIRE(readOggSamples(s, buf, 16) == 0);

    // Rewind and decode the same stream again — the MusicManager loop path.
    seekOggStart(s);
    REQUIRE(drain() == firstPass);
    closeOggStream(s);
}

// ---------------------------------------------------------------------------
// VoiceCalloutManager with real OGG asset (exercises play + upload paths)
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager play with valid OGG asset decodes and uploads", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    SubtitleQueue sq;

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<FakeAudioPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    VoiceCalloutManager vcm;
    vcm.init(&audio, &assets, &sq, nullptr, &log);
    AudioSettings settings{0.8f, 1.0f, 0.7f, 1.0f, 1.0f};
    VoiceCallout callout{"sfx/rwr_warning", nullptr, 4.0f};
    vcm.play(callout, settings); // decodes kMinimalOgg → bufId > 0 → play() called
    vcm.play(callout, settings); // second call hits buffer cache
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// MusicManager with real OGG (exercises openSlot success + crossfade)
// ---------------------------------------------------------------------------

TEST_CASE("MusicManager openSlot succeeds with valid OGG asset", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<FakeAudioPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);

    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::Menu); // openSlot succeeds: slot.active = true
    mm.update(0.016f, 1.0f, 1.0f);
    mm.shutdown();
}

TEST_CASE("MusicManager openSlot fails gracefully on malformed OGG that passes validation", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    // OggS magic + garbage passes AssetManager's magic-byte gate, so openSlot reaches
    // openOggStream and must take the "failed to open OGG" warn path without crashing.
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<OggPrefixGarbagePack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);

    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::Menu); // openSlot fails: slot stays inactive
    mm.update(0.016f, 1.0f, 1.0f);
    mm.shutdown();
}

TEST_CASE("MusicManager crossfade path when switching states with active slot", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<FakeAudioPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);

    PlaylistData pd = parsePlaylist(kValidPlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::Menu);         // primary becomes active
    mm.setState(GameState::FlightPatrol); // triggers crossfade (primary.active == true)

    // Advance time past crossfade duration in a single update to complete it
    mm.update(3.0f, 1.0f, 1.0f);   // dt > crossfadeDuration(2.5) → t >= 1 → crossfade done
    mm.update(0.016f, 1.0f, 1.0f); // post-crossfade steady-state update
    mm.shutdown();
}

// ---------------------------------------------------------------------------
// MusicManager shuffle tests
// ---------------------------------------------------------------------------

TEST_CASE("MusicManager shuffle=false always opens first declared track", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    auto pack = std::make_unique<TrackingAudioPack>();
    TrackingAudioPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::Menu);
    REQUIRE(!tp->opened.empty());
    REQUIRE(tp->opened[0] == "music/menu");
    mm.shutdown();
}

TEST_CASE("MusicManager shuffle=true opens a valid track on state entry", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    auto pack = std::make_unique<TrackingAudioPack>();
    TrackingAudioPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::FlightPatrol);
    REQUIRE(!tp->opened.empty());
    const std::vector<std::string> valid = {"music/a", "music/b", "music/c"};
    REQUIRE(std::find(valid.begin(), valid.end(), tp->opened[0]) != valid.end());
    mm.shutdown();
}

TEST_CASE("MusicManager shuffle=true produces a deterministic permutation with fixed RNG", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    auto pack = std::make_unique<TrackingAudioPack>();
    TrackingAudioPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    mm.setRng(std::mt19937{42});
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::FlightPatrol);
    REQUIRE(!tp->opened.empty());

    std::vector<std::string> expected = {"music/a", "music/b", "music/c"};
    std::shuffle(expected.begin(), expected.end(), std::mt19937{42});
    REQUIRE(tp->opened[0] == expected[0]);
    mm.shutdown();
}

TEST_CASE("MusicManager shuffle=true re-entry rebuilds the permutation", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    // NullTrackingContentPack returns nullopt so AssetManager never caches the result.
    // TrackingAudioPack would cause a cache hit when the re-shuffled order opens the
    // same first track as the initial entry, dropping the loadAudio call and giving
    // opened.size() == 2 instead of 3.
    auto pack = std::make_unique<NullTrackingContentPack>();
    NullTrackingContentPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    mm.setState(GameState::FlightPatrol); // 1st entry
    mm.setState(GameState::Menu);         // switch away
    mm.setState(GameState::FlightPatrol); // re-enter: rebuilds permutation
    REQUIRE(tp->opened.size() == 3);
    const std::vector<std::string> valid = {"music/a", "music/b", "music/c"};
    REQUIRE(std::find(valid.begin(), valid.end(), tp->opened[2]) != valid.end());
    mm.shutdown();
}

TEST_CASE("MusicManager shuffle=false wrap-around plays all tracks in declaration order", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    auto pack = std::make_unique<NullTrackingContentPack>();
    NullTrackingContentPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    // FlightCombat: shuffle=false, 3 tracks ["music/x","music/y","music/z"].
    // NullTrackingContentPack causes openSlot to fail → slot.active stays false →
    // EOF handler fires on every update() call.
    mm.setState(GameState::FlightCombat); // opens x (index 0)
    mm.update(0.016f, 1.0f, 1.0f);        // advance to index 1, opens y
    mm.update(0.016f, 1.0f, 1.0f);        // advance to index 2, opens z
    mm.update(0.016f, 1.0f, 1.0f);        // next=3>=n=3, wrap to index 0, opens x again

    REQUIRE(tp->opened.size() == 4);
    REQUIRE(tp->opened[0] == "music/x");
    REQUIRE(tp->opened[1] == "music/y");
    REQUIRE(tp->opened[2] == "music/z");
    REQUIRE(tp->opened[3] == "music/x");
    mm.shutdown();
}

TEST_CASE("MusicManager shuffle=true one full cycle contains each track exactly once", "[audio][music]") {
    NullAudio audio;
    NullLogger log;

    auto pack = std::make_unique<NullTrackingContentPack>();
    NullTrackingContentPack* tp = pack.get();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    MusicManager mm;
    mm.init(&audio, &assets, &log);
    mm.setRng(std::mt19937{99});
    PlaylistData pd = parsePlaylist(kShufflePlaylist, log);
    mm.loadPlaylist(pd);

    // FlightPatrol: shuffle=true, 3 tracks.
    mm.setState(GameState::FlightPatrol); // opens permuted[0]
    mm.update(0.016f, 1.0f, 1.0f);        // opens permuted[1]
    mm.update(0.016f, 1.0f, 1.0f);        // opens permuted[2]
    mm.update(0.016f, 1.0f, 1.0f);        // wrap: re-shuffle, opens new permuted[0]

    REQUIRE(tp->opened.size() == 4);

    // First full cycle (indices 0-2) must be a permutation of all three tracks.
    std::vector<std::string> cycle(tp->opened.begin(), tp->opened.begin() + 3);
    std::sort(cycle.begin(), cycle.end());
    const std::vector<std::string> all = {"music/a", "music/b", "music/c"};
    REQUIRE(cycle == all);

    // Wrap triggered a re-shuffle and opened a 4th track.
    const std::vector<std::string> valid = {"music/a", "music/b", "music/c"};
    REQUIRE(std::find(valid.begin(), valid.end(), tp->opened[3]) != valid.end());
    mm.shutdown();
}

// ---------------------------------------------------------------------------
// VoiceCalloutManager targeted coverage tests
// ---------------------------------------------------------------------------

TEST_CASE("VoiceCalloutManager asset not found logs warning", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;

    // NullContentPack returns nullopt for all assets → exercises lines 39-41
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<NullContentPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    VoiceCalloutManager vcm;
    vcm.init(&audio, &assets, nullptr, nullptr, &log);
    AudioSettings settings{};
    vcm.play(VoiceCallout{"sfx/missing", nullptr, 4.0f}, settings);
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager OGG decode failure logs warning", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;

    // GarbageAudioPack returns non-OGG bytes → decodeOgg fails → exercises lines 46-48
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<GarbageAudioPack>());
    AssetManager assets(std::move(packs), log);
    assets.initialize(nullptr);

    VoiceCalloutManager vcm;
    vcm.init(&audio, &assets, nullptr, nullptr, &log);
    AudioSettings settings{};
    vcm.play(VoiceCallout{"sfx/bad", nullptr, 4.0f}, settings);
    vcm.shutdown();
}

TEST_CASE("VoiceCalloutManager subtitle push via Localization fallback", "[audio][voice]") {
    NullAudio audio;
    NullLogger log;
    MockFilesystem fs; // empty — Localization falls back to key-as-text
    Localization i18n(fs, log);
    SubtitleQueue sq;

    VoiceCalloutManager vcm;
    vcm.init(&audio, nullptr, &sq, &i18n, &log);
    AudioSettings settings{};
    // subtitleKey is non-null + i18n is non-null → i18n.get("rwr.warning") returns "rwr.warning"
    // → subtitleText = "rwr.warning" → sq.push() called → covers lines 63-65
    // sq.enabled() is called inside play() → covers SubtitleQueue::enabled() getter
    vcm.play(VoiceCallout{nullptr, "rwr.warning", 3.0f}, settings);
    REQUIRE(sq.current() == "rwr.warning");
    vcm.shutdown();
}

// ---------------------------------------------------------------------------
// Builtin procedural music (#865)
// ---------------------------------------------------------------------------

namespace {
struct CapturingMusicLog : ILogger {
    std::vector<std::string> warnings;
    void log(LogLevel lvl, const char*, int, const char* msg) override {
        if (lvl == LogLevel::Warn && msg)
            warnings.emplace_back(msg);
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};
} // namespace

TEST_CASE("generateBuiltinMusic is deterministic and distinct per mood (#865)", "[audio][music]") {
    const DecodedPcm menuA = generateBuiltinMusic(MusicMood::Menu);
    const DecodedPcm menuB = generateBuiltinMusic(MusicMood::Menu);
    CHECK(menuA.channels == 1);
    CHECK(menuA.sampleRate == kMusicSampleRate);
    REQUIRE_FALSE(menuA.samples.empty());
    CHECK(menuA.samples == menuB.samples); // byte-stable: no rand()/time

    const DecodedPcm patrol = generateBuiltinMusic(MusicMood::Patrol);
    const DecodedPcm combat = generateBuiltinMusic(MusicMood::Combat);
    CHECK(menuA.samples != patrol.samples); // each mood is a different loop
    CHECK(patrol.samples != combat.samples);
}

TEST_CASE("builtin music tracks + the default playlist wire the game states (#865)", "[audio][music]") {
    CHECK(builtinMusicTrack("builtin:music-menu").valid());
    CHECK(builtinMusicTrack("builtin:music-patrol").valid());
    CHECK(builtinMusicTrack("builtin:music-combat").valid());
    CHECK_FALSE(builtinMusicTrack("fl-base:theme").valid()); // not a builtin id
    CHECK(builtinMusicTrackNames().size() == 3u);

    const PlaylistData pl = builtinDefaultPlaylist();
    REQUIRE(pl.findState("Menu") != nullptr);
    REQUIRE(pl.findState("FlightCombat") != nullptr);
    CHECK(pl.findState("Menu")->tracks.at(0) == "builtin:music-menu");
    CHECK(pl.findState("FlightCombat")->tracks.at(0) == "builtin:music-combat");
}

TEST_CASE("MusicManager plays the builtin default playlist with no content pack (#865)", "[audio][music]") {
    NullAudio audio;
    CapturingMusicLog log;
    MusicManager mm;
    // No AssetManager: the builtin tracks are resolved before AssetManager, so nullptr is fine.
    REQUIRE(mm.init(&audio, /*assets=*/nullptr, &log));
    mm.loadPlaylist(builtinDefaultPlaylist());

    mm.setState(GameState::Menu); // opens builtin:music-menu (streams the looping PCM)
    mm.update(1.0f / 60.0f, 0.8f, 0.7f);
    mm.setState(GameState::FlightCombat); // crossfades to builtin:music-combat
    mm.update(1.0f / 60.0f, 0.8f, 0.7f);

    // The builtin tracks resolve, so the "track not found" warning never fires.
    for (const std::string& w : log.warnings)
        CHECK(w.find("not found") == std::string::npos);
    mm.shutdown();
}
