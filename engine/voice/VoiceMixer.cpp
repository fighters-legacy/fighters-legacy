// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice/VoiceMixer.h"

#include "ILogger.h"
#include "voice/VoiceActivity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fl {

VoiceMixer::VoiceMixer() = default;
VoiceMixer::~VoiceMixer() {
    shutdown();
}

bool VoiceMixer::init(IAudio* audio, ILogger* logger) {
    m_audio = audio;
    m_logger = logger;
    m_pcm.assign(static_cast<std::size_t>(kVoiceFrameSamples), 0);
    m_nets.clear();
    for (auto& def : builtinRadioNets())
        m_nets.add(def);
    return m_audio != nullptr;
}

void VoiceMixer::shutdown() {
    reset();
    m_audio = nullptr;
    m_logger = nullptr;
}

void VoiceMixer::setNets(const RadioNetTable& nets) {
    m_nets = nets;
}

void VoiceMixer::applySettings(const VoiceSettings& voice, const AudioSettings& audio) {
    m_voice = voice;
    m_audioSettings = audio;
    const auto target = static_cast<uint32_t>(std::clamp(voice.jitterTargetFrames, 1, 12));
    for (auto& s : m_streams)
        s->jitter.setDepth(target, VoiceJitterBuffer::kDefaultMaxDepth);
}

float VoiceMixer::netVolume(uint8_t netId) const {
    const RadioNetDef* def = m_nets.byIndex(netId);
    const float authored = def ? def->gain : 1.f;
    const float user = netId < m_voice.netVolume.size() ? m_voice.netVolume[netId] : 1.f;
    return authored * user * m_audioSettings.masterVolume * m_audioSettings.voiceChatVolume;
}

VoiceMixer::Stream* VoiceMixer::findStream(uint32_t peerId, uint8_t netId) {
    for (auto& s : m_streams) {
        if (s->peerId == peerId && s->netId == netId)
            return s.get();
    }
    return nullptr;
}

VoiceMixer::Stream* VoiceMixer::createStream(uint32_t peerId, uint8_t netId) {
    if (!m_audio)
        return nullptr;
    auto decoder = createVoiceDecoder();
    if (!decoder) {
        if (m_logger)
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__, "voice: decoder creation failed");
        return nullptr;
    }
    auto s = std::make_unique<Stream>();
    s->peerId = peerId;
    s->netId = netId;
    s->decoder = std::move(decoder);
    s->jitter.setDepth(static_cast<uint32_t>(std::clamp(m_voice.jitterTargetFrames, 1, 12)),
                       VoiceJitterBuffer::kDefaultMaxDepth);
    s->source = m_audio->createSource();
    if (!s->source)
        return nullptr;
    for (int i = 0; i < kStreamBuffers; ++i) {
        s->buffers[i] = m_audio->allocStreamBuffer();
        s->freeList[s->freeCount++] = s->buffers[i];
    }
    m_streams.push_back(std::move(s));
    return m_streams.back().get();
}

void VoiceMixer::destroyStream(Stream& s) {
    if (!m_audio)
        return;
    if (s.source) {
        m_audio->detachBuffers(s.source);
        m_audio->destroySource(s.source);
        s.source = 0;
    }
    for (int i = 0; i < kStreamBuffers; ++i) {
        if (s.buffers[i])
            m_audio->freeBuffer(s.buffers[i]);
        s.buffers[i] = 0;
    }
    s.freeCount = 0;
    s.queued = 0;
}

void VoiceMixer::reset() {
    for (auto& s : m_streams)
        destroyStream(*s);
    m_streams.clear();
    m_active.clear();
    m_duckGain = 1.f;
}

void VoiceMixer::onFrame(uint32_t senderPeerId, uint32_t senderEntityIdx, uint8_t netId, uint16_t seq,
                         std::span<const uint8_t> payload, bool start, bool end) {
    if (!m_voice.enabled || !m_audio)
        return;
    if (!m_nets.byIndex(netId))
        return; // a net this client does not know: the server's table is authoritative
    if (payload.size() > kMaxVoicePayloadBytes)
        return; // the length cap, enforced before the bytes can reach libopus

    Stream* s = findStream(senderPeerId, netId);
    if (!s) {
        if (payload.empty())
            return; // a stray end-marker for a stream we never had: nothing to close
        s = createStream(senderPeerId, netId);
        if (!s)
            return;
    }
    s->entityIdx = senderEntityIdx;
    if (start) {
        // A new transmission: clear codec + de-jitter state so the previous burst's tail cannot
        // bleed into this one, and so the sequence numbering restarts cleanly.
        s->jitter.reset();
        s->decoder->reset();
        s->ending = false;
    }
    if (end)
        s->ending = true;
    if (!payload.empty())
        s->jitter.push(seq, payload);
}

void VoiceMixer::pumpStream(Stream& s, float dt) {
    if (!m_audio || !s.source)
        return;

    // Recycle buffers OpenAL has finished with.
    const int processed = m_audio->processedBufferCount(s.source);
    if (processed > 0) {
        AudioBufferId done[kStreamBuffers]{};
        const int take = std::min(processed, kStreamBuffers);
        m_audio->unqueueProcessed(s.source, done, take);
        for (int i = 0; i < take; ++i) {
            if (done[i] && s.freeCount < kStreamBuffers)
                s.freeList[s.freeCount++] = done[i];
        }
        s.queued = std::max(0, s.queued - take);
    }

    bool produced = false;
    while (s.queued < kTargetQueued && s.freeCount > 0) {
        // Concealment bridges LOSS. Once the sender has told us the transmission is over there is
        // nothing left to conceal, and running PLC anyway would append ~160 ms of invented speech
        // to every single transmission — and keep the stream alive long past its end.
        if (s.ending && s.jitter.size() == 0)
            break;
        m_payload.clear();
        const auto status = s.jitter.pop(m_payload);
        if (status == VoiceJitterBuffer::Pop::Empty)
            break;
        const int samples =
            s.decoder->decode(status == VoiceJitterBuffer::Pop::Conceal ? std::span<const uint8_t>{}
                                                                        : std::span<const uint8_t>(m_payload),
                              std::span<int16_t>(m_pcm));
        if (samples <= 0)
            break;
        produced = true;

        // Level meter for the HUD, measured on the DECODED audio so it reflects what the listener
        // actually hears rather than what the sender's mic saw.
        const float rms =
            VoiceActivityGate::rms(std::span<const int16_t>(m_pcm.data(), static_cast<std::size_t>(samples)));
        s.level = s.level * 0.7f + rms * 0.3f;

        const AudioBufferId buf = s.freeList[--s.freeCount];
        m_audio->queueBuffer(s.source, buf, m_pcm.data(), static_cast<std::size_t>(samples) * sizeof(int16_t),
                             kVoiceSampleRate, kVoiceChannels);
        ++s.queued;
    }

    if (produced) {
        s.idleSec = 0.f;
        if (!s.started || !m_audio->isPlaying(s.source)) {
            // Also covers underrun: OpenAL stops a source that runs out of queued buffers, and it
            // must be resumed once refilled or the stream goes permanently silent.
            m_audio->resume(s.source);
            s.started = true;
        }
    } else {
        s.idleSec += dt;
        s.level *= 0.8f;
    }
}

void VoiceMixer::placeStream(Stream& s, const glm::dvec3& cameraOrigin, const SpeakerPositionFn& posFn) {
    if (!m_audio || !s.source)
        return;
    const RadioNetDef* def = m_nets.byIndex(s.netId);
    const bool wantPositional = def && def->positional;

    glm::dvec3 world{};
    const bool haveWorld = wantPositional && posFn && s.entityIdx != 0xFFFFFFFFu && posFn(s.entityIdx, world);

    m_audio->setGain(s.source, netVolume(s.netId));
    if (haveWorld) {
        const glm::vec3 rel(world - cameraOrigin);
        m_audio->setSourceRelative(s.source, false);
        m_audio->setPosition(s.source, rel.x, rel.y, rel.z);
        m_audio->setReferenceDistance(s.source, kPositionalReferenceM);
        m_audio->setMaxDistance(s.source, def->rangeM > 0.f ? def->rangeM : kPositionalMaxM);
        m_audio->setRolloffFactor(s.source, 1.f);
    } else {
        // Head-locked: a radio is in your headset. Also the fallback when the speaker's aircraft is
        // not in this client's snapshot — better a centred voice than a voice at the world origin.
        m_audio->setSourceRelative(s.source, true);
        m_audio->setPosition(s.source, 0.f, 0.f, 0.f);
        m_audio->setRolloffFactor(s.source, 0.f);
    }
}

void VoiceMixer::update(float dt, const glm::dvec3& cameraOrigin, const SpeakerPositionFn& posFn) {
    m_active.clear();
    if (!m_audio)
        return;

    for (auto& sp : m_streams) {
        Stream& s = *sp;
        pumpStream(s, dt);
        placeStream(s, cameraOrigin, posFn);
        if (s.queued > 0 || s.jitter.size() > 0) {
            const RadioNetDef* def = m_nets.byIndex(s.netId);
            m_active.push_back(ActiveSpeaker{s.peerId, s.netId, def && def->positional, s.level});
        }
    }

    // Retire streams that have been silent past the timeout. An `ending` stream is retired as soon
    // as it drains, so a closed transmission releases its source immediately rather than holding it
    // for the full idle window.
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        Stream& s = **it;
        const bool drained = s.queued == 0 && s.jitter.size() == 0;
        if (drained && (s.ending || s.idleSec >= kStreamIdleTimeoutS)) {
            destroyStream(s);
            it = m_streams.erase(it);
        } else {
            ++it;
        }
    }

    // Ducking (#925): one smoothed envelope for the whole radio, not per net — the listener's ear
    // does not care which net is live, only that someone is talking.
    const float target = m_active.empty() ? 1.f : std::clamp(1.f - m_voice.duckingAmount, 0.f, 1.f);
    const float rate = target < m_duckGain ? kDuckAttackPerSec : kDuckReleasePerSec;
    const float step = rate * dt;
    if (std::fabs(target - m_duckGain) <= step)
        m_duckGain = target;
    else
        m_duckGain += (target > m_duckGain ? step : -step);
}

} // namespace fl
