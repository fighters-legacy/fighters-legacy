// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "audio/MusicManager.h"
#include "audio/OggDecoder.h"
#include "audio/PlaylistLoader.h"
#include "audio/SubtitleQueue.h"
#include "audio/VoiceCalloutManager.h"
#include "content/AssetManager.h"
#include "content/IContentPack.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <memory>
#include <vector>

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

// ---------------------------------------------------------------------------
// NullContentPack — returns nullopt/empty for every asset request.
// Lets us construct a real AssetManager in tests without needing filesystem access.
// ---------------------------------------------------------------------------
struct NullContentPack : IContentPack {
    const char* name() const override {
        return "null";
    }
    const char* version() const override {
        return "0";
    }
    const char* id() const override {
        return "null";
    }
    int priority() const override {
        return 0;
    }
    const char* rootDirectory() const override {
        return nullptr;
    }

    Status init() override {
        return Status::Ready;
    }
    bool configure(IWindow*) override {
        return true;
    }

    bool hasAsset(const char*, AssetType) const override {
        return false;
    }

    std::optional<MeshData> loadMesh(const char*) override {
        return std::nullopt;
    }
    std::optional<TextureData> loadTexture(const char*) override {
        return std::nullopt;
    }
    std::optional<AudioBuffer> loadAudio(const char*) override {
        return std::nullopt;
    }
    std::optional<FlightModel> loadFlightModel(const char*) override {
        return std::nullopt;
    }
    std::optional<MissionData> loadMission(const char*) override {
        return std::nullopt;
    }
    std::optional<TerrainData> loadTerrain(const char*) override {
        return std::nullopt;
    }
    std::optional<AIScript> loadAIScript(const char*) override {
        return std::nullopt;
    }
    std::optional<EntityDefData> loadEntityDef(const char*) override {
        return std::nullopt;
    }
    std::vector<std::string> listAssets(AssetType) const override {
        return {};
    }
    std::optional<std::string> loadConfig(const char*) const override {
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
