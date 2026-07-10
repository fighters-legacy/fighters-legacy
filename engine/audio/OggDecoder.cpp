// SPDX-License-Identifier: GPL-3.0-or-later
//
// OGG Vorbis decode over libvorbis (vorbisfile). Content-pack audio is
// attacker-controlled, so this path must be memory-safe on malformed input
// (fuzzed by fuzz_ogg): the reference decoder replaced stb_vorbis, which is
// a trusted-input decoder and crashes on adversarial streams (#723). The
// wrapper adds its own defense-in-depth on top: a full-decode output cap
// (decompression bombs), a channel/sample-rate sanity envelope, a bound on
// consecutive OV_HOLE retries, and a chained-bitstream format-change guard.
#include "audio/OggDecoder.h"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>

namespace fl {

namespace {

// ov_read output byte order: 0 = little-endian, 1 = big-endian (host order here).
constexpr int kHostBigEndian = (std::endian::native == std::endian::big) ? 1 : 0;

// Consecutive OV_HOLE results tolerated inside one readOggSamples call before
// giving up — keeps an adversarial stream from spinning the main thread.
constexpr int kMaxConsecutiveHoles = 64;

// Sanity envelope enforced at open. Vorbis legally allows up to 255 channels,
// which multiplies decoded output size; OpenAL only plays mono/stereo anyway.
constexpr int kMinChannels = 1;
constexpr int kMaxChannels = 8;
constexpr long kMinSampleRate = 8000;
constexpr long kMaxSampleRate = 192000;

// fread-style memory data source for ov_open_callbacks.
struct MemSource {
    const uint8_t* data{nullptr};
    std::size_t size{0};
    std::size_t pos{0};
};

std::size_t memRead(void* ptr, std::size_t size, std::size_t nmemb, void* datasource) {
    auto* src = static_cast<MemSource*>(datasource);
    if (size == 0 || nmemb == 0)
        return 0;
    const std::size_t avail = src->size - src->pos;
    const std::size_t items = std::min(nmemb, avail / size);
    const std::size_t bytes = items * size;
    if (bytes > 0) {
        std::memcpy(ptr, src->data + src->pos, bytes);
        src->pos += bytes;
    }
    return items;
}

int memSeek(void* datasource, ogg_int64_t offset, int whence) {
    auto* src = static_cast<MemSource*>(datasource);
    ogg_int64_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = static_cast<ogg_int64_t>(src->pos);
        break;
    case SEEK_END:
        base = static_cast<ogg_int64_t>(src->size);
        break;
    default:
        return -1;
    }
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(src->size))
        return -1;
    src->pos = static_cast<std::size_t>(target);
    return 0;
}

long memTell(void* datasource) {
    const auto* src = static_cast<MemSource*>(datasource);
    if (src->pos > static_cast<std::size_t>(LONG_MAX))
        return -1;
    return static_cast<long>(src->pos);
}

// close_func is nullptr: the caller owns the byte buffer (openOggStream contract).
constexpr ov_callbacks kMemCallbacks = {memRead, memSeek, nullptr, memTell};

} // namespace

struct OggStream {
    MemSource src;
    OggVorbis_File vf{};
    OggStreamInfo info;
};

OggStream* openOggStream(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return nullptr;

    auto* stream = new OggStream();
    stream->src = MemSource{bytes.data(), bytes.size(), 0};

    // On failure ov_open_callbacks cleans up after itself — ov_clear must NOT
    // be called on a failed open (vorbisfile API contract).
    if (ov_open_callbacks(&stream->src, &stream->vf, nullptr, 0, kMemCallbacks) != 0) {
        delete stream;
        return nullptr;
    }

    const vorbis_info* vi = ov_info(&stream->vf, -1);
    if (!vi || vi->channels < kMinChannels || vi->channels > kMaxChannels || vi->rate < kMinSampleRate ||
        vi->rate > kMaxSampleRate) {
        ov_clear(&stream->vf);
        delete stream;
        return nullptr;
    }

    stream->info.sampleRate = static_cast<int>(vi->rate);
    stream->info.channels = vi->channels;
    return stream;
}

OggStreamInfo getOggStreamInfo(const OggStream* stream) {
    if (!stream)
        return {};
    return stream->info;
}

int readOggSamples(OggStream* stream, int16_t* buf, int numSamples) {
    if (!stream || !buf || numSamples <= 0)
        return 0;

    const int channels = stream->info.channels;
    const int frameBytes = channels * static_cast<int>(sizeof(int16_t));
    int framesDecoded = 0;
    int consecutiveHoles = 0;

    while (framesDecoded < numSamples) {
        char* dst = reinterpret_cast<char*>(buf + static_cast<std::size_t>(framesDecoded) * channels);
        const int wantBytes = (numSamples - framesDecoded) * frameBytes;
        int bitstream = 0;
        const long ret = ov_read(&stream->vf, dst, wantBytes, kHostBigEndian, 2, 1, &bitstream);

        if (ret == OV_HOLE) {
            // Corrupt page gap — vorbisfile skipped it; retry, bounded.
            if (++consecutiveHoles > kMaxConsecutiveHoles)
                break;
            continue;
        }
        if (ret <= 0)
            break; // EOF (0) or unrecoverable error (negative)
        consecutiveHoles = 0;

        // Chained-bitstream guard: if this read came from a link whose format
        // differs from the opening link, discard it and treat as end-of-stream —
        // format-switched PCM must never reach the audio backend.
        const vorbis_info* vi = ov_info(&stream->vf, bitstream);
        if (!vi || vi->channels != channels || static_cast<int>(vi->rate) != stream->info.sampleRate)
            break;

        framesDecoded += static_cast<int>(ret) / frameBytes;
    }
    return framesDecoded;
}

void seekOggStart(OggStream* stream) {
    if (stream)
        (void)ov_pcm_seek(&stream->vf, 0);
}

void closeOggStream(OggStream* stream) {
    if (!stream)
        return;
    ov_clear(&stream->vf);
    delete stream;
}

DecodedPcm decodeOgg(std::span<const uint8_t> bytes, std::size_t maxTotalSamples) {
    OggStreamPtr stream(openOggStream(bytes));
    if (!stream)
        return {};

    constexpr int kChunkFrames = 4096;
    const int channels = stream->info.channels;

    DecodedPcm result;
    result.sampleRate = stream->info.sampleRate;
    result.channels = channels;

    std::vector<int16_t> chunk(static_cast<std::size_t>(kChunkFrames) * channels);
    for (;;) {
        const int frames = readOggSamples(stream.get(), chunk.data(), kChunkFrames);
        if (frames <= 0)
            break;
        const std::size_t newElements = static_cast<std::size_t>(frames) * channels;
        if (result.samples.size() + newElements > maxTotalSamples)
            return {}; // decompression bomb — reject, never truncate
        result.samples.insert(result.samples.end(), chunk.begin(), chunk.begin() + newElements);
    }

    if (result.samples.empty())
        return {};
    return result;
}

} // namespace fl
