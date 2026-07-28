// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoiceCommandCapture.h"

#include "IAudioCapture.h"
#include "ILogger.h"
#include "voice/VoiceCodec.h" // kVoiceSampleRate — one capture-rate convention for both voice paths

namespace fl {

namespace {

// An order is a few words. Anything past this is someone leaning on the key, and transcribing a
// minute of room audio to look for "rejoin" wastes CPU the tier exists to conserve.
constexpr std::size_t kMaxUtteranceSamples = kVoiceSampleRate * 8; // 8 s

// Below this there is no speech, only a keypress. Submitting it would spend a transcription on
// silence and produce a confident wrong answer.
constexpr std::size_t kMinUtteranceSamples = kVoiceSampleRate / 4; // 250 ms

constexpr std::size_t kDrainChunk = 4096;

} // namespace

VoiceCommandCapture::VoiceCommandCapture(IAudioCapture* capture, ISpeechToText* stt, ILogger* logger)
    : m_capture(capture), m_stt(stt), m_logger(logger) {
    if (m_stt)
        m_stt->setHandler(this);
    m_scratch.resize(kDrainChunk);
}

VoiceCommandCapture::~VoiceCommandCapture() {
    // Deregister before this object dies: the transcriber outlives it (the game owns both) and a
    // completion firing into a destroyed handler is a crash in teardown.
    if (m_stt)
        m_stt->setHandler(nullptr);
}

bool VoiceCommandCapture::available() const {
    return m_capture != nullptr && m_stt != nullptr && m_stt->available();
}

void VoiceCommandCapture::update(bool keyHeld, bool uiFocused) {
    if (!available()) {
        m_listening = false;
        return;
    }

    // A chat box or a menu must never key the mic — the same rule VoiceChat's gate follows, and for
    // the same reason: typing "engage" into chat should not also say it.
    const bool wantListen = keyHeld && !uiFocused;

    if (wantListen && !m_listening)
        startListening();
    else if (!wantListen && m_listening)
        stopAndSubmit();

    if (m_listening) {
        drainCapture();
        // A stuck key is a real thing. Cut the utterance at the cap and transcribe what there is,
        // rather than growing a buffer until something else notices.
        if (m_buffer.size() >= kMaxUtteranceSamples)
            stopAndSubmit();
    }
}

void VoiceCommandCapture::startListening() {
    m_buffer.clear();
    if (!m_capture->start()) {
        if (m_logger)
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__, "voice command: capture device would not start");
        return;
    }
    m_listening = true;
    if (m_onStatus)
        m_onStatus("Listening...");
}

void VoiceCommandCapture::drainCapture() {
    for (;;) {
        const std::size_t got = m_capture->read(m_scratch.data(), m_scratch.size());
        if (got == 0)
            break;
        m_buffer.insert(m_buffer.end(), m_scratch.begin(), m_scratch.begin() + static_cast<std::ptrdiff_t>(got));
        if (m_buffer.size() >= kMaxUtteranceSamples)
            break;
    }
}

void VoiceCommandCapture::stopAndSubmit() {
    m_listening = false;
    drainCapture(); // whatever was mid-flight when the key came up is still part of the utterance
    m_capture->stop();

    if (m_buffer.size() < kMinUtteranceSamples) {
        m_buffer.clear();
        if (m_onStatus)
            m_onStatus({});
        return;
    }

    // One utterance at a time. A second press while the first is still transcribing replaces it:
    // the pilot has moved on, and delivering the older order later would be worse than dropping it.
    if (m_pending != 0)
        m_stt->cancel(m_pending);

    m_pending = m_stt->submit(m_buffer.data(), m_buffer.size(), kVoiceSampleRate);
    m_buffer.clear();
    if (m_onStatus)
        m_onStatus(m_pending != 0 ? "..." : std::string{});
}

void VoiceCommandCapture::onTranscript(SttRequestId id, SttStatus status, const std::string& text,
                                       const std::string& error) {
    if (id != m_pending)
        return; // a cancelled or superseded utterance
    m_pending = 0;

    if (status != SttStatus::Success) {
        if (m_logger && status == SttStatus::Error && !error.empty())
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__, ("voice command: " + error).c_str());
        if (m_onStatus)
            m_onStatus({});
        return;
    }

    // The transcript is untrusted in the same sense chat is — it is whatever a microphone in a room
    // produced. It goes through the deterministic matcher and NOWHERE near a command string.
    const auto cmd = ai::matchWingmanPhrase(text);
    if (!cmd) {
        // Declining is a real outcome, not an error: below threshold or ambiguous between two
        // commands. Say so, so the pilot repeats themselves instead of wondering if the key works.
        if (m_onStatus)
            m_onStatus("Say again?");
        return;
    }

    if (m_onStatus)
        m_onStatus({});
    if (m_onCommand)
        m_onCommand(*cmd);
}

} // namespace fl
