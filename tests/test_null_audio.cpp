// SPDX-License-Identifier: GPL-3.0-or-later
//
// The no-op IAudio contract (#1117). NullAudio is what the game falls back to on a machine with no
// audio endpoint, and what every audio unit test drives, so the properties callers rely on are
// worth stating once: handles look like SUCCESS, nothing plays, and every call is safe.
#include <catch2/catch_test_macros.hpp>

#include "NullAudio.h"

#include <set>

using namespace fl;

TEST_CASE("NullAudio hands out handles that read as success, not failure", "[audio][null]") {
    // Callers compare a handle against 0 to detect an allocation failure and take an error path.
    // Returning 0 here would send well-written code down that path on a machine that is merely
    // quiet — the degradation has to look like a working device, not a broken one.
    NullAudio audio;
    REQUIRE(audio.init());

    std::set<AudioSourceId> sources;
    std::set<AudioBufferId> buffers;
    for (int i = 0; i < 8; ++i) {
        const AudioSourceId src = audio.createSource();
        const AudioBufferId buf = audio.uploadBuffer("pcm", 3, 44100, 1);
        const AudioBufferId stream = audio.allocStreamBuffer();
        CHECK(src != 0);
        CHECK(buf != 0);
        CHECK(stream != 0);
        sources.insert(src);
        buffers.insert(buf);
        buffers.insert(stream);
    }
    // Unique, because callers key their own state on them.
    CHECK(sources.size() == 8);
    CHECK(buffers.size() == 16);
}

TEST_CASE("NullAudio never reports playback and never recycles a stream buffer", "[audio][null]") {
    NullAudio audio;
    REQUIRE(audio.init());

    const AudioSourceId src = audio.createSource();
    const AudioBufferId buf = audio.allocStreamBuffer();
    audio.queueBuffer(src, buf, "pcm", 3, 44100, 1);
    audio.play(src, buf);

    // Nothing is playing, so nothing finishes: a streaming consumer keeps its slot open and simply
    // never advances, which is the same shape as a track that has not reached its end.
    CHECK_FALSE(audio.isPlaying(src));
    CHECK(audio.processedBufferCount(src) == 0);
    CHECK(audio.getLastError() == nullptr);
}

TEST_CASE("every NullAudio call is safe, including on handles it never issued", "[audio][null]") {
    // The degraded path runs the ordinary game code, which will stop, retune and free sources in
    // whatever order the session takes — including during a teardown after a failed init.
    NullAudio audio;
    REQUIRE(audio.init());

    const float vec[3] = {1.f, 2.f, 3.f};
    for (AudioSourceId s : {AudioSourceId{0}, AudioSourceId{99}}) {
        audio.stop(s);
        audio.pause(s);
        audio.resume(s);
        audio.setLooping(s, true);
        audio.setPitch(s, 1.5f);
        audio.setGain(s, 0.5f);
        audio.setPosition(s, 1.f, 2.f, 3.f);
        audio.setVelocity(s, 1.f, 2.f, 3.f);
        audio.setReferenceDistance(s, 10.f);
        audio.setMaxDistance(s, 100.f);
        audio.setRolloffFactor(s, 1.f);
        audio.setSourceRelative(s, true);
        audio.detachBuffers(s);
        audio.unqueueProcessed(s, nullptr, 0);
        audio.destroySource(s);
    }
    audio.freeBuffer(0);
    audio.setListenerTransform(vec, vec, vec);
    audio.setListenerVelocity(vec);
    audio.shutdown();
    audio.shutdown(); // idempotent — Game tears the platform down explicitly and again on destruct

    SUCCEED("no call on a null audio device may fault");
}
