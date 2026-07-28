// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-layer snapshot payload compression (#775). zstd level 1 over the assembled per-peer
// snapshot payload — everything after the raw 24-byte MsgWorldSnapshotHeader. Transport-agnostic
// by design: enet6's range coder compressed on the wire and GNS (the default internet transport)
// does not compress at all, so the engine owns the codec and both backends benefit identically.
//
// Chosen against a captured 64-client corpus on the real snapshot stream (idle/weave/aggressive):
// zstd L1 removes 81/28/20 % of payload at ~7 us/packet compress, ~3.5 us decompress — beating
// enet6's range coder on every pattern (LZ4 missed idle parity badly; zstd L3 bought +2 % for
// +20 % CPU). See docs/developer/network-protocol.md for the framing and #775 for the measurement.
//
// Lives in its own target (engine-compress): engine-protocol must stay stdlib-only (layering rule
// 2, cmake/layering.cmake), so the zstd dependency hangs off engine-net/the game client instead.
//
// Threading: both functions keep a reused zstd context per calling thread (thread_local), so they
// are safe from the JobSystem's parallel per-peer pass and deterministic for identical input —
// the #512 serial-equivalence guarantee (byte-identical buffers across worker counts) holds.
#pragma once

#include <cstdint>
#include <vector>

namespace fl {

// Payloads below this size are sent raw — the zstd frame header alone eats the win.
inline constexpr std::size_t kMinSnapshotCompressBytes = 128u;

// Upper bound a receiver accepts for the claimed decompressed size. Generous (an unlimited-budget
// snapshot of ~20k entities is ~600 KiB) but hard — a malformed/hostile header cannot make the
// client allocate or decompress unbounded output.
inline constexpr std::size_t kMaxSnapshotPayloadBytes = 4u * 1024u * 1024u;

// Compress `src[0..srcSize)` into `dst` (resized to the compressed length). Returns the compressed
// size, or 0 when compression is not worthwhile (srcSize below kMinSnapshotCompressBytes, the
// result would not be strictly smaller than the input, or a codec error) — the caller then sends
// the payload raw with the flag clear.
std::size_t compressSnapshotPayload(const uint8_t* src, std::size_t srcSize, std::vector<uint8_t>& dst);

// Decompress `src[0..srcSize)` into `dst` (resized to exactly `claimedUncompressed`). Fails closed
// (returns false, dst unspecified) when the claim exceeds kMaxSnapshotPayloadBytes, the frame is
// malformed, or the decoded length differs from the claim.
bool decompressSnapshotPayload(const uint8_t* src, std::size_t srcSize, uint32_t claimedUncompressed,
                               std::vector<uint8_t>& dst);

} // namespace fl
