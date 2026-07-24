// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------------------------
// Opus voice codec (#531)
// ---------------------------------------------------------------------------------------------
// One fixed operating point, chosen once and shared by every net, because a negotiated codec
// configuration is a matrix of failure modes for a benefit nobody hears: 48 kHz mono, 20 ms
// frames, VOIP application, VBR with in-band FEC. The server relays frames WITHOUT decoding, so
// the frame geometry is not the server's business at all — it is a client-to-client contract.
//
// 48 kHz because that is the only rate OpenAL, SDL capture and Opus all agree on natively; going
// to 16 kHz would trade a resample on both ends for bytes Opus does not actually spend (at 24
// kbps the encoder allocates by bandwidth, not sample rate).
//
// 20 ms because it is Opus's sweet spot: 10 ms doubles packet overhead for ~4 ms of latency, 40 ms
// puts a full extra frame of delay in front of a call that already crossed the internet.
// ---------------------------------------------------------------------------------------------

inline constexpr int kVoiceSampleRate = 48000;
inline constexpr int kVoiceChannels = 1;
inline constexpr int kVoiceFrameMs = 20;
inline constexpr int kVoiceFrameSamples = kVoiceSampleRate / 1000 * kVoiceFrameMs; // 960
inline constexpr int kVoiceDefaultBitrate = 24000;                                 // bits/s, VOIP-clean speech

// Hard cap on one encoded frame, enforced on BOTH sides. A client decodes bytes that reached it
// via an untrusted peer through a server that never inspected them, so the length is the one thing
// we can and must bound before it reaches libopus. 400 B/frame = 160 kbit/s, far above any
// configuration we will ever encode at, and still one comfortable MTU with the header.
inline constexpr std::size_t kMaxVoicePayloadBytes = 400;

// Bounds the number of consecutive lost frames a decoder will conceal before it gives up and goes
// silent: 8 x 20 ms = 160 ms of PLC. Beyond that, concealment is inventing speech that was never
// said, which is worse than a gap.
inline constexpr int kMaxVoiceConcealFrames = 8;

// One encoded voice frame in flight. `seq` is per-(speaker, net) and wraps; the jitter buffer
// resolves ordering with a half-window comparison, so wrapping is not a special case.
struct VoiceFrame {
    uint16_t seq{0};
    std::vector<uint8_t> payload; // Opus, <= kMaxVoicePayloadBytes; empty = an explicit silence marker
};

// Encodes 20 ms mono PCM frames to Opus. Construct via createVoiceEncoder(); a null return means
// libopus refused the configuration (in practice: never), and the caller degrades to no voice.
class VoiceEncoder {
  public:
    virtual ~VoiceEncoder() = default;

    // pcm must be exactly kVoiceFrameSamples int16 samples. Returns the encoded byte count written
    // to `out` (which must have room for kMaxVoicePayloadBytes), or 0 on failure.
    virtual std::size_t encode(std::span<const int16_t> pcm, std::span<uint8_t> out) = 0;

    // Target bitrate in bits/s, clamped to a sane speech range. Applied immediately.
    virtual void setBitrate(int bitsPerSecond) = 0;

    // Enable Opus in-band FEC and tell the encoder the expected loss, so it spends bits on
    // redundancy proportional to the link's actual behaviour rather than a guess baked in at build
    // time. Driven from the transport's measured loss.
    virtual void setExpectedPacketLoss(int percent) = 0;

    // Reset codec memory (a new transmission starts from silence, not from the tail of the last).
    virtual void reset() = 0;
};

// Decodes one remote speaker's stream. One decoder per active speaker: Opus is stateful, and
// sharing a decoder across speakers smears one voice's spectral state into the next.
class VoiceDecoder {
  public:
    virtual ~VoiceDecoder() = default;

    // Decode one frame into exactly kVoiceFrameSamples samples. Returns the sample count written
    // (kVoiceFrameSamples on success, 0 on failure). Pass an EMPTY payload to run packet-loss
    // concealment for a frame that never arrived.
    virtual int decode(std::span<const uint8_t> payload, std::span<int16_t> pcm) = 0;

    virtual void reset() = 0;
};

[[nodiscard]] std::unique_ptr<VoiceEncoder> createVoiceEncoder(int bitrate = kVoiceDefaultBitrate);
[[nodiscard]] std::unique_ptr<VoiceDecoder> createVoiceDecoder();

// Human-readable libopus version, for the startup banner and bug reports.
[[nodiscard]] const char* voiceCodecVersion() noexcept;

} // namespace fl
