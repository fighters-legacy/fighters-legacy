// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice/VoiceChat.h"

#include "ILogger.h"

#include <algorithm>
#include <cstdio>

namespace fl {

VoiceChat::VoiceChat() {
    for (auto& def : builtinRadioNets())
        m_nets.add(def);
    deriveNetSelection();
}

VoiceChat::~VoiceChat() {
    shutdown();
}

bool VoiceChat::init(IAudioCapture* capture, IAudio* audio, ILogger* logger) {
    m_capture = capture;
    m_logger = logger;
    m_encoded.assign(kMaxVoicePayloadBytes, 0);
    m_captureBuf.assign(static_cast<std::size_t>(kVoiceFrameSamples) * 8, 0);
    m_accum.clear();
    m_accum.reserve(static_cast<std::size_t>(kVoiceFrameSamples) * 2);

    m_mixer.init(audio, logger);
    m_mixer.setNets(m_nets);

    m_encoder = createVoiceEncoder(m_voice.bitrate);
    if (!m_encoder && m_logger)
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__, "voice: opus encoder unavailable; listen-only");

    if (m_logger) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "voice: codec %s, %d Hz mono, %d ms frames", voiceCodecVersion(),
                      kVoiceSampleRate, kVoiceFrameMs);
        m_logger->log(LogLevel::Info, __FILE__, __LINE__, msg);
    }
    return m_encoder != nullptr || audio != nullptr;
}

void VoiceChat::shutdown() {
    reset();
    closeCapture();
    m_mixer.shutdown();
    m_encoder.reset();
    m_capture = nullptr;
    m_logger = nullptr;
}

void VoiceChat::setNets(const RadioNetTable& nets) {
    m_nets = nets;
    m_mixer.setNets(nets);
    deriveNetSelection();
}

void VoiceChat::deriveNetSelection() {
    m_primaryNet = m_nets.defaultIndex();
    m_secondaryNet = kInvalidRadioNet;
    // The secondary key wants the OTHER net a pilot lives on: the flight net if the server defines
    // one, otherwise simply the next net along, so the key is never dead.
    for (std::size_t i = 0; i < m_nets.size(); ++i) {
        if (m_nets.nets()[i].kind == RadioNetKind::Flight && static_cast<uint8_t>(i) != m_primaryNet) {
            m_secondaryNet = static_cast<uint8_t>(i);
            break;
        }
    }
    if (m_secondaryNet == kInvalidRadioNet && m_nets.size() > 1)
        m_secondaryNet = static_cast<uint8_t>((m_primaryNet + 1) % m_nets.size());
}

void VoiceChat::cyclePrimaryNet() {
    if (m_nets.size() < 2)
        return;
    m_primaryNet = static_cast<uint8_t>((m_primaryNet + 1) % m_nets.size());
    if (m_primaryNet == m_secondaryNet)
        m_primaryNet = static_cast<uint8_t>((m_primaryNet + 1) % m_nets.size());
}

std::string_view VoiceChat::netName(uint8_t netId) const noexcept {
    if (const RadioNetDef* def = m_nets.byIndex(netId))
        return def->name;
    return {};
}

void VoiceChat::applySettings(const VoiceSettings& voice, const AudioSettings& audio) {
    const bool transmitChanged = voice.transmitEnabled != m_voice.transmitEnabled || voice.enabled != m_voice.enabled ||
                                 voice.inputDevice != m_voice.inputDevice;
    m_voice = voice;
    m_audioSettings = audio;
    m_gate.setMode(voice.keyMode);
    m_gate.setThreshold(voice.voxThreshold);
    if (m_encoder)
        m_encoder->setBitrate(voice.bitrate);
    m_mixer.applySettings(voice, audio);
    if (transmitChanged) {
        // A device change must actually re-open the device, not just remember the name.
        closeCapture();
        m_captureAttempted = false;
    }
}

void VoiceChat::setExpectedPacketLoss(int percent) {
    if (m_encoder)
        m_encoder->setExpectedPacketLoss(percent);
}

void VoiceChat::ensureCapture() {
    if (m_captureReady || m_captureAttempted || !m_capture)
        return;
    m_captureAttempted = true; // one attempt per settings change: no per-frame retry storm
    if (!m_capture->init(kVoiceSampleRate, kVoiceChannels, m_voice.inputDevice)) {
        const char* err = m_capture->getLastError();
        m_captureError = err ? err : "no capture device";
        if (m_logger)
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__,
                          ("voice: capture unavailable (" + m_captureError + "); listen-only").c_str());
        return;
    }
    m_captureReady = true;
    m_captureError.clear();
    if (m_logger)
        m_logger->log(LogLevel::Info, __FILE__, __LINE__,
                      ("voice: capture device '" + m_capture->currentDevice() + "'").c_str());
}

void VoiceChat::closeCapture() {
    if (m_capture && m_captureReady) {
        m_capture->stop();
        m_capture->shutdown();
    }
    m_captureReady = false;
}

void VoiceChat::processFrame(std::span<const int16_t> pcm, bool keyHeld) {
    const auto res = m_gate.evaluate(pcm, keyHeld);
    m_micLevel = m_micLevel * 0.7f + VoiceActivityGate::rms(pcm) * 0.3f;

    if (res.ended) {
        // Explicit end-of-transmission marker: no audio, just the boundary the receiver turns into
        // a squelch tail (#925). Deriving it from a receive timeout would put the squelch late.
        if (m_sink && m_txNet != kInvalidRadioNet)
            m_sink(m_txNet, m_seq++, {}, /*start=*/false, /*end=*/true);
        m_txNet = kInvalidRadioNet;
        return;
    }
    if (!res.transmit)
        return;
    if (!m_encoder || !m_sink)
        return;

    if (res.started) {
        // update() has already latched m_txNet for this burst (primary vs secondary key); do NOT
        // re-derive it here or holding the secondary key would still transmit on the primary net.
        m_encoder->reset();
        m_seq = 0;
    }
    if (m_txNet == kInvalidRadioNet)
        return;

    const std::size_t n = m_encoder->encode(pcm, std::span<uint8_t>(m_encoded));
    if (n == 0)
        return;
    m_sink(m_txNet, m_seq++, std::span<const uint8_t>(m_encoded.data(), n), res.started, /*end=*/false);
}

void VoiceChat::update(float dt, bool pttPrimary, bool pttSecondary, bool uiFocused, const glm::dvec3& cameraOrigin,
                       const SpeakerPositionFn& posFn) {
    // Receive side runs even with transmit disabled — listening is the common case.
    m_mixer.update(dt, cameraOrigin, posFn);

    if (!m_voice.enabled || !m_voice.transmitEnabled || uiFocused) {
        if (m_gate.close() && m_sink && m_txNet != kInvalidRadioNet)
            m_sink(m_txNet, m_seq++, {}, false, /*end=*/true);
        if (!m_gate.isOpen())
            m_txNet = kInvalidRadioNet;
        if (m_captureReady && m_capture->isCapturing())
            m_capture->stop();
        m_micLevel *= 0.8f;
        return;
    }

    ensureCapture();
    if (!m_captureReady)
        return;

    const bool keyHeld = pttPrimary || pttSecondary;
    // The net for a NEW transmission. Secondary wins when both keys are down; the net is latched
    // for the whole burst, so releasing one key mid-sentence cannot re-route the tail of it onto
    // another net (which would deliver half a sentence to the wrong people).
    if (!m_gate.isOpen())
        m_txNet = pttSecondary ? m_secondaryNet : m_primaryNet;

    // In PTT mode the device only runs while keyed — an always-hot mic is both a privacy problem
    // and a needless wakeup every frame. VOX and Open modes need it continuously to hear onset.
    const bool wantCapture = (m_voice.keyMode != VoiceKeyMode::PushToTalk) || keyHeld || m_gate.isOpen();
    if (wantCapture && !m_capture->isCapturing())
        m_capture->start();
    else if (!wantCapture && m_capture->isCapturing())
        m_capture->stop();

    if (!m_capture->isCapturing()) {
        m_micLevel *= 0.8f;
        return;
    }

    // Drain everything the device produced since the last update and cut it into 20 ms frames.
    while (true) {
        const std::size_t got = m_capture->read(m_captureBuf.data(), m_captureBuf.size());
        if (got == 0)
            break;
        m_accum.insert(m_accum.end(), m_captureBuf.begin(), m_captureBuf.begin() + static_cast<std::ptrdiff_t>(got));
        if (got < m_captureBuf.size())
            break;
    }

    const auto frameSamples = static_cast<std::size_t>(kVoiceFrameSamples);
    // Bound the backlog: a stall (alt-tab, a load hitch) must not queue seconds of stale speech
    // that then floods the net at once. Keep at most half a second and drop the oldest.
    const std::size_t maxBacklog = frameSamples * 25;
    if (m_accum.size() > maxBacklog)
        m_accum.erase(m_accum.begin(), m_accum.end() - static_cast<std::ptrdiff_t>(maxBacklog));

    std::size_t consumed = 0;
    while (m_accum.size() - consumed >= frameSamples) {
        int16_t* frame = m_accum.data() + consumed;
        if (m_voice.micGain != 1.0f) {
            for (std::size_t i = 0; i < frameSamples; ++i) {
                const float v = static_cast<float>(frame[i]) * m_voice.micGain;
                frame[i] = static_cast<int16_t>(std::clamp(v, -32768.f, 32767.f));
            }
        }
        processFrame(std::span<const int16_t>(frame, frameSamples), keyHeld);
        consumed += frameSamples;
    }
    if (consumed > 0)
        m_accum.erase(m_accum.begin(), m_accum.begin() + static_cast<std::ptrdiff_t>(consumed));
}

void VoiceChat::onRemoteFrame(uint32_t senderPeerId, uint32_t senderEntityIdx, uint8_t netId, uint16_t seq,
                              std::span<const uint8_t> payload, bool start, bool end) {
    m_mixer.onFrame(senderPeerId, senderEntityIdx, netId, seq, payload, start, end);
}

void VoiceChat::reset() {
    if (m_gate.close() && m_sink && m_txNet != kInvalidRadioNet)
        m_sink(m_txNet, m_seq++, {}, false, /*end=*/true);
    m_txNet = kInvalidRadioNet;
    m_seq = 0;
    m_accum.clear();
    m_micLevel = 0.f;
    if (m_captureReady && m_capture && m_capture->isCapturing())
        m_capture->stop();
    m_mixer.reset();
}

} // namespace fl
