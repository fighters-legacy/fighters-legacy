// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice/VoiceCodec.h"

#include <opus.h>

#include <algorithm>

namespace fl {
namespace {

class OpusVoiceEncoder final : public VoiceEncoder {
  public:
    explicit OpusVoiceEncoder(OpusEncoder* enc) : m_enc(enc) {}
    ~OpusVoiceEncoder() override {
        if (m_enc)
            opus_encoder_destroy(m_enc);
    }
    OpusVoiceEncoder(const OpusVoiceEncoder&) = delete;
    OpusVoiceEncoder& operator=(const OpusVoiceEncoder&) = delete;

    std::size_t encode(std::span<const int16_t> pcm, std::span<uint8_t> out) override {
        if (!m_enc || pcm.size() != static_cast<std::size_t>(kVoiceFrameSamples) || out.empty())
            return 0;
        const auto cap = static_cast<opus_int32>(std::min(out.size(), kMaxVoicePayloadBytes));
        const opus_int32 n = opus_encode(m_enc, pcm.data(), kVoiceFrameSamples, out.data(), cap);
        return n > 0 ? static_cast<std::size_t>(n) : 0u;
    }

    void setBitrate(int bitsPerSecond) override {
        if (!m_enc)
            return;
        const auto br = static_cast<opus_int32>(std::clamp(bitsPerSecond, 6000, 128000));
        opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(br));
    }

    void setExpectedPacketLoss(int percent) override {
        if (!m_enc)
            return;
        const int p = std::clamp(percent, 0, 100);
        opus_encoder_ctl(m_enc, OPUS_SET_INBAND_FEC(p > 0 ? 1 : 0));
        opus_encoder_ctl(m_enc, OPUS_SET_PACKET_LOSS_PERC(p));
    }

    void reset() override {
        if (m_enc)
            opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
    }

  private:
    OpusEncoder* m_enc{nullptr};
};

class OpusVoiceDecoder final : public VoiceDecoder {
  public:
    explicit OpusVoiceDecoder(OpusDecoder* dec) : m_dec(dec) {}
    ~OpusVoiceDecoder() override {
        if (m_dec)
            opus_decoder_destroy(m_dec);
    }
    OpusVoiceDecoder(const OpusVoiceDecoder&) = delete;
    OpusVoiceDecoder& operator=(const OpusVoiceDecoder&) = delete;

    int decode(std::span<const uint8_t> payload, std::span<int16_t> pcm) override {
        if (!m_dec || pcm.size() < static_cast<std::size_t>(kVoiceFrameSamples))
            return 0;
        // An oversized payload never reaches libopus: the length cap is the only validation we can
        // do on bytes that crossed the network unread by the server.
        if (payload.size() > kMaxVoicePayloadBytes)
            return 0;
        const int n = payload.empty()
                          // Empty payload = packet-loss concealment for a frame that never arrived.
                          ? opus_decode(m_dec, nullptr, 0, pcm.data(), kVoiceFrameSamples, /*decode_fec=*/0)
                          : opus_decode(m_dec, payload.data(), static_cast<opus_int32>(payload.size()), pcm.data(),
                                        kVoiceFrameSamples, /*decode_fec=*/0);
        return n > 0 ? n : 0;
    }

    void reset() override {
        if (m_dec)
            opus_decoder_ctl(m_dec, OPUS_RESET_STATE);
    }

  private:
    OpusDecoder* m_dec{nullptr};
};

} // namespace

std::unique_ptr<VoiceEncoder> createVoiceEncoder(int bitrate) {
    int err = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(kVoiceSampleRate, kVoiceChannels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc)
        return nullptr;
    auto wrapper = std::make_unique<OpusVoiceEncoder>(enc);
    wrapper->setBitrate(bitrate);
    // 5% assumed loss until the transport reports otherwise: cheap insurance that makes the first
    // seconds of a call on a lossy link intelligible instead of shredded.
    wrapper->setExpectedPacketLoss(5);
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    // Band-limit at the encoder: everything above 8 kHz is spent on breath and cockpit noise, and
    // the radio DSP (#925) throws it away anyway.
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(6));
    return wrapper;
}

std::unique_ptr<VoiceDecoder> createVoiceDecoder() {
    int err = OPUS_OK;
    OpusDecoder* dec = opus_decoder_create(kVoiceSampleRate, kVoiceChannels, &err);
    if (err != OPUS_OK || !dec)
        return nullptr;
    return std::make_unique<OpusVoiceDecoder>(dec);
}

const char* voiceCodecVersion() noexcept {
    return opus_get_version_string();
}

} // namespace fl
