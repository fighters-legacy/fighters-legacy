// SPDX-License-Identifier: GPL-3.0-or-later
#include "OALAudio.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

static std::vector<int16_t> silentMono(int samples = 100) {
    return std::vector<int16_t>(samples, 0);
}

TEST_CASE("OALAudio init and shutdown round-trip", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");
    CHECK(audio.getLastError() == nullptr);
    audio.shutdown();
    // second shutdown must be idempotent
    audio.shutdown();
}

TEST_CASE("OALAudio buffer lifecycle", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    auto pcm = silentMono();
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(buf != 0);
    CHECK(audio.getLastError() == nullptr);

    audio.freeBuffer(buf);
    CHECK(audio.getLastError() == nullptr);

    audio.shutdown();
}

TEST_CASE("OALAudio stereo buffer upload", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    std::vector<int16_t> pcm(200, 0); // 2 channels × 100 samples
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 2);
    REQUIRE(buf != 0);
    CHECK(audio.getLastError() == nullptr);

    audio.freeBuffer(buf);
    audio.shutdown();
}

TEST_CASE("OALAudio source lifecycle", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);
    CHECK(!audio.isPlaying(src));

    audio.destroySource(src);
    CHECK(audio.getLastError() == nullptr);

    audio.shutdown();
}

TEST_CASE("OALAudio play and stop", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    // 100 ms of silence — long enough for AL_PLAYING state to register
    auto pcm = silentMono(kRate / 10);
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(buf != 0);

    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.play(src, buf);
    CHECK(audio.isPlaying(src));

    audio.stop(src);
    CHECK(!audio.isPlaying(src));

    audio.destroySource(src);
    audio.freeBuffer(buf);
    audio.shutdown();
}

TEST_CASE("OALAudio pause and resume", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    auto pcm = silentMono(kRate / 10);
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(buf != 0);
    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.play(src, buf);
    CHECK(audio.isPlaying(src));

    audio.pause(src);
    CHECK(!audio.isPlaying(src)); // AL_PAUSED, not AL_PLAYING

    audio.resume(src);
    CHECK(audio.isPlaying(src));

    audio.stop(src);
    audio.destroySource(src);
    audio.freeBuffer(buf);
    audio.shutdown();
}

TEST_CASE("OALAudio play replaces buffer on active source", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    auto pcm = silentMono(kRate / 10);
    AudioBufferId bufA = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    AudioBufferId bufB = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(bufA != 0);
    REQUIRE(bufB != 0);
    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.play(src, bufA);
    CHECK(audio.isPlaying(src));

    // Rebind to bufB while playing — must not latch AL_INVALID_OPERATION
    audio.play(src, bufB);
    CHECK(audio.getLastError() == nullptr);

    audio.stop(src);
    audio.destroySource(src);
    audio.freeBuffer(bufA);
    audio.freeBuffer(bufB);
    audio.shutdown();
}

TEST_CASE("OALAudio setLooping setPitch setGain", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.setLooping(src, true);
    CHECK(audio.getLastError() == nullptr);

    audio.setPitch(src, 1.5f);
    CHECK(audio.getLastError() == nullptr);

    audio.setGain(src, 0.5f);
    CHECK(audio.getLastError() == nullptr);

    audio.setLooping(src, false);
    audio.destroySource(src);
    audio.shutdown();
}

TEST_CASE("OALAudio source relative (non-positional) mode", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const int kRate = 44100;
    auto pcm = silentMono(kRate / 10);
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(buf != 0);
    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.setSourceRelative(src, true);
    audio.setPosition(src, 0.0f, 0.0f, 0.0f);
    audio.setRolloffFactor(src, 0.0f);

    audio.play(src, buf);
    CHECK(audio.getLastError() == nullptr);

    audio.stop(src);
    audio.setSourceRelative(src, false);
    CHECK(audio.getLastError() == nullptr);

    audio.destroySource(src);
    audio.freeBuffer(buf);
    audio.shutdown();
}

TEST_CASE("OALAudio invalid source ID is a silent no-op", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    const AudioSourceId bad = 9999;

    const int kRate = 44100;
    auto pcm = silentMono();
    AudioBufferId buf = audio.uploadBuffer(pcm.data(), pcm.size() * sizeof(int16_t), kRate, 1);
    REQUIRE(buf != 0);

    // All of these must silently no-op without setting m_lastError
    audio.play(bad, buf);
    CHECK(audio.getLastError() == nullptr);
    audio.stop(bad);
    CHECK(audio.getLastError() == nullptr);
    audio.pause(bad);
    CHECK(audio.getLastError() == nullptr);
    audio.resume(bad);
    CHECK(audio.getLastError() == nullptr);
    CHECK(!audio.isPlaying(bad));
    audio.setLooping(bad, true);
    CHECK(audio.getLastError() == nullptr);
    audio.setPitch(bad, 1.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setGain(bad, 1.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setSourceRelative(bad, true);
    CHECK(audio.getLastError() == nullptr);
    audio.setPosition(bad, 0.0f, 0.0f, 0.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setVelocity(bad, 0.0f, 0.0f, 0.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setReferenceDistance(bad, 1.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setMaxDistance(bad, 100.0f);
    CHECK(audio.getLastError() == nullptr);
    audio.setRolloffFactor(bad, 1.0f);
    CHECK(audio.getLastError() == nullptr);

    audio.freeBuffer(buf);
    audio.shutdown();
}

TEST_CASE("OALAudio freeBuffer with unknown ID is a no-op", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    audio.freeBuffer(0);
    audio.freeBuffer(9999);
    // must not crash and must not set an error
    CHECK(audio.getLastError() == nullptr);

    audio.shutdown();
}

TEST_CASE("OALAudio 3D spatial params and listener", "[audio]") {
    OALAudio audio;
    if (!audio.init())
        SKIP("no audio device");

    AudioSourceId src = audio.createSource();
    REQUIRE(src != 0);

    audio.setPosition(src, 10.0f, 0.0f, -5.0f);
    CHECK(audio.getLastError() == nullptr);

    audio.setVelocity(src, 50.0f, 0.0f, 0.0f);
    CHECK(audio.getLastError() == nullptr);

    audio.setReferenceDistance(src, 2.0f);
    CHECK(audio.getLastError() == nullptr);

    audio.setMaxDistance(src, 200.0f);
    CHECK(audio.getLastError() == nullptr);

    audio.setRolloffFactor(src, 1.0f);
    CHECK(audio.getLastError() == nullptr);

    const float pos[3] = {5.0f, 0.0f, 0.0f};
    const float forward[3] = {0.0f, 0.0f, -1.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    audio.setListenerTransform(pos, forward, up);
    CHECK(audio.getLastError() == nullptr);

    const float vel[3] = {100.0f, 0.0f, 0.0f};
    audio.setListenerVelocity(vel);
    CHECK(audio.getLastError() == nullptr);

    audio.destroySource(src);
    audio.shutdown();
}
