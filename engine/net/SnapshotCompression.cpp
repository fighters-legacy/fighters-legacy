// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnapshotCompression.h"

#include <zstd.h>

namespace fl {

namespace {

// Compression level: L1 won the corpus bench (#775) — see SnapshotCompression.h. Not a knob; a
// different level is a measured decision, not a config default someone flips blind.
constexpr int kSnapshotZstdLevel = 1;

// One reused context per thread. The JobSystem workers are persistent, so each pays the context
// allocation once; zstd's output is a pure function of (input, level) regardless of what the
// context compressed before, so reuse cannot break the #512 byte-identical guarantee.
struct CompressCtx {
    ZSTD_CCtx* ctx{ZSTD_createCCtx()};
    ~CompressCtx() {
        ZSTD_freeCCtx(ctx);
    }
};
struct DecompressCtx {
    ZSTD_DCtx* ctx{ZSTD_createDCtx()};
    ~DecompressCtx() {
        ZSTD_freeDCtx(ctx);
    }
};

} // namespace

std::size_t compressSnapshotPayload(const uint8_t* src, std::size_t srcSize, std::vector<uint8_t>& dst) {
    if (src == nullptr || srcSize < kMinSnapshotCompressBytes)
        return 0u;
    thread_local CompressCtx tls;
    if (tls.ctx == nullptr)
        return 0u;
    dst.resize(ZSTD_compressBound(srcSize));
    const std::size_t written = ZSTD_compressCCtx(tls.ctx, dst.data(), dst.size(), src, srcSize, kSnapshotZstdLevel);
    if (ZSTD_isError(written) || written >= srcSize)
        return 0u; // not strictly smaller (or codec error) — caller sends raw
    dst.resize(written);
    return written;
}

bool decompressSnapshotPayload(const uint8_t* src, std::size_t srcSize, uint32_t claimedUncompressed,
                               std::vector<uint8_t>& dst) {
    if (src == nullptr || srcSize == 0u || claimedUncompressed == 0u || claimedUncompressed > kMaxSnapshotPayloadBytes)
        return false;
    thread_local DecompressCtx tls;
    if (tls.ctx == nullptr)
        return false;
    dst.resize(claimedUncompressed);
    const std::size_t decoded = ZSTD_decompressDCtx(tls.ctx, dst.data(), dst.size(), src, srcSize);
    // The claim is part of the framing: a frame that decodes to any other length is malformed.
    return !ZSTD_isError(decoded) && decoded == claimedUncompressed;
}

} // namespace fl
