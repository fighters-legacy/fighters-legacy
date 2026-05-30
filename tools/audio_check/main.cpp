// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "OALAudio.h"
#include "audio/OggDecoder.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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
    std::printf("audio_check: %s — press Ctrl-C to skip\n", label);
    while (audio.isPlaying(src) && !g_quit)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_quit) {
        std::printf("audio_check: skipped\n");
        g_quit = false;
        return false;
    }
    std::printf("audio_check: done\n");
    return true;
}

// Reports OGG metadata and verifies decode. Returns 0 on success, 1 on failure.
static int checkOgg(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        std::fprintf(stderr, "error: '%s' is empty\n", path);
        return 1;
    }

    // Open streaming decoder to read metadata cheaply.
    OggStream* stream = openOggStream(bytes);
    if (!stream) {
        std::fprintf(stderr, "error: '%s' is not a valid OGG Vorbis file\n", path);
        return 1;
    }

    OggStreamInfo info = getOggStreamInfo(stream);

    // Count total samples by decoding fully (gives exact duration).
    int16_t scratch[4096];
    int64_t totalSamples = 0;
    int16_t peakAbs = 0;
    int decoded = 0;
    while ((decoded = readOggSamples(stream, scratch, 2048)) > 0) {
        totalSamples += decoded;
        for (int i = 0; i < decoded * info.channels; ++i) {
            int16_t v = scratch[i] < 0 ? static_cast<int16_t>(-scratch[i]) : scratch[i];
            if (v > peakAbs)
                peakAbs = v;
        }
    }
    closeOggStream(stream);

    float durationSec =
        info.sampleRate > 0 ? static_cast<float>(totalSamples) / static_cast<float>(info.sampleRate) : 0.0f;
    float peakDb = peakAbs > 0 ? 20.0f * std::log10(static_cast<float>(peakAbs) / 32767.0f) : -999.0f;

    std::printf("file:        %s\n", path);
    std::printf("sample_rate: %d Hz\n", info.sampleRate);
    std::printf("channels:    %d\n", info.channels);
    std::printf("duration:    %.2f s\n", durationSec);
    std::printf("total_frames: %" PRId64 "\n", totalSamples);
    std::printf("peak:        %.1f dBFS\n", peakDb);
    return 0;
}

static void printHelp() {
    std::printf("Usage: audio_check [--check-ogg <file.ogg>] [--version] [--help]\n"
                "\n"
                "Without arguments: runs interactive OpenAL hardware demo (3D + head-locked).\n"
                "With --check-ogg: decodes the OGG file and reports metadata; no audio playback.\n"
                "\n"
                "Options:\n"
                "  --check-ogg <file>  Validate and report metadata for an OGG Vorbis file\n"
                "  --version, -v       Show version and exit\n"
                "  --help, -h          Show this help and exit\n");
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printHelp();
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("audio_check 0.0.1\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--check-ogg") == 0 && i + 1 < argc) {
            return checkOgg(argv[++i]);
        }
    }

    // --- Interactive OpenAL hardware demo ---
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    OALAudio audio;
    if (!audio.init()) {
        std::fprintf(stderr, "audio_check: init failed: %s\n", audio.getLastError());
        return 1;
    }
    std::printf("audio_check: OpenAL device opened\n");

    const int kRate = 44100;
    const float lPos[3] = {0.0f, 0.0f, 0.0f};
    const float lForward[3] = {0.0f, 0.0f, -1.0f};
    const float lUp[3] = {0.0f, 1.0f, 0.0f};
    audio.setListenerTransform(lPos, lForward, lUp);

    // Demo 1: 3D positional source (440 Hz, left of listener).
    auto pcm3d = makeSine(440.0f, 1.5f, kRate);
    AudioBufferId buf3d = audio.uploadBuffer(pcm3d.data(), pcm3d.size() * sizeof(int16_t), kRate, 1);
    AudioSourceId src3d = audio.createSource();
    if (!buf3d || !src3d) {
        std::fprintf(stderr, "audio_check: setup failed: %s\n", audio.getLastError());
        audio.shutdown();
        return 1;
    }
    audio.setPosition(src3d, -5.0f, 0.0f, 0.0f);
    audio.setReferenceDistance(src3d, 1.0f);
    audio.setMaxDistance(src3d, 50.0f);
    audio.setRolloffFactor(src3d, 1.0f);
    audio.setGain(src3d, 0.7f);
    playAndWait(audio, src3d, buf3d, "3D positional 440 Hz (should sound left)");

    // Demo 2: 2D head-locked source (880 Hz, music path).
    auto pcm2d = makeSine(880.0f, 1.5f, kRate);
    AudioBufferId buf2d = audio.uploadBuffer(pcm2d.data(), pcm2d.size() * sizeof(int16_t), kRate, 1);
    AudioSourceId src2d = audio.createSource();
    if (!buf2d || !src2d) {
        std::fprintf(stderr, "audio_check: setup failed: %s\n", audio.getLastError());
        audio.shutdown();
        return 1;
    }
    audio.setSourceRelative(src2d, true);
    audio.setPosition(src2d, 0.0f, 0.0f, 0.0f);
    audio.setRolloffFactor(src2d, 0.0f);
    audio.setGain(src2d, 0.5f);
    playAndWait(audio, src2d, buf2d, "2D head-locked 880 Hz (should sound centred)");

    audio.destroySource(src3d);
    audio.destroySource(src2d);
    audio.freeBuffer(buf3d);
    audio.freeBuffer(buf2d);
    audio.shutdown();
    std::printf("audio_check: all demos complete\n");
    return 0;
}
