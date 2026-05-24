// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "OALAudio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static std::atomic<bool> g_quit{false};
static void onSignal(int) {
    g_quit = true;
}

static std::vector<int16_t> makeSine(float hz, float durationSec, int rate) {
    int n = static_cast<int>(rate * durationSec);
    std::vector<int16_t> buf(n);
    for (int i = 0; i < n; ++i)
        buf[i] = static_cast<int16_t>(
            32767.0f * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / static_cast<float>(rate)));
    return buf;
}

static bool playAndWait(OALAudio& audio, AudioSourceId src, AudioBufferId buf, const char* label) {
    audio.play(src, buf);
    std::printf("audio_test: %s — press Ctrl-C to skip\n", label);
    while (audio.isPlaying(src) && !g_quit)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_quit) {
        std::printf("audio_test: skipped\n");
        g_quit = false; // allow next demo to run
        return false;
    }
    std::printf("audio_test: done\n");
    return true;
}

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    OALAudio audio;
    if (!audio.init()) {
        std::fprintf(stderr, "audio_test: init failed: %s\n", audio.getLastError());
        return 1;
    }
    std::printf("audio_test: OpenAL device opened\n");

    const int kRate = 44100;

    // Set up listener at origin, facing -Z
    const float lPos[3] = {0.0f, 0.0f, 0.0f};
    const float lForward[3] = {0.0f, 0.0f, -1.0f};
    const float lUp[3] = {0.0f, 1.0f, 0.0f};
    audio.setListenerTransform(lPos, lForward, lUp);

    // --- Demo 1: 3D positional source (440 Hz, left of listener) ---
    auto pcm3d = makeSine(440.0f, 1.5f, kRate);
    AudioBufferId buf3d = audio.uploadBuffer(pcm3d.data(), pcm3d.size() * sizeof(int16_t), kRate, 1);
    AudioSourceId src3d = audio.createSource();
    if (!buf3d || !src3d) {
        std::fprintf(stderr, "audio_test: setup failed: %s\n", audio.getLastError());
        audio.shutdown();
        return 1;
    }
    audio.setPosition(src3d, -5.0f, 0.0f, 0.0f);
    audio.setReferenceDistance(src3d, 1.0f);
    audio.setMaxDistance(src3d, 50.0f);
    audio.setRolloffFactor(src3d, 1.0f);
    audio.setGain(src3d, 0.7f);
    playAndWait(audio, src3d, buf3d, "3D positional 440 Hz (should sound left)");

    // --- Demo 2: 2D non-positional / head-locked source (880 Hz, music path) ---
    auto pcm2d = makeSine(880.0f, 1.5f, kRate);
    AudioBufferId buf2d = audio.uploadBuffer(pcm2d.data(), pcm2d.size() * sizeof(int16_t), kRate, 1);
    AudioSourceId src2d = audio.createSource();
    if (!buf2d || !src2d) {
        std::fprintf(stderr, "audio_test: setup failed: %s\n", audio.getLastError());
        audio.shutdown();
        return 1;
    }
    audio.setSourceRelative(src2d, true); // head-locked
    audio.setPosition(src2d, 0.0f, 0.0f, 0.0f);
    audio.setRolloffFactor(src2d, 0.0f); // no distance attenuation
    audio.setGain(src2d, 0.5f);
    playAndWait(audio, src2d, buf2d, "2D head-locked 880 Hz (should sound centred)");

    audio.destroySource(src3d);
    audio.destroySource(src2d);
    audio.freeBuffer(buf3d);
    audio.freeBuffer(buf2d);
    audio.shutdown();
    std::printf("audio_test: all demos complete\n");
    return 0;
}
