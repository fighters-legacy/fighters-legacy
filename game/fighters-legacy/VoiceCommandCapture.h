// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ISpeechToText.h"
#include "ai/WingmanPhraseMatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fl {
class IAudioCapture;
class ILogger;
} // namespace fl

// The client half of the deterministic voice-command tier (#935).
//
// Hold the WingmanVoiceCommand key, say "two, engage bandits", release. The audio is captured
// locally, transcribed locally, matched locally, and what leaves the machine is a
// MsgWingmanCommand ordinal — the SAME packet the radio menu produces. Nothing about the pilot's
// voice ever reaches the server or another player on this path.
//
// That is deliberate and is the difference between this and the voice PTT keys: those transmit
// audio to other players, this one does not transmit audio at all.
//
// EVERY STEP DEGRADES INDEPENDENTLY. No capture device, no STT backend, no model, an unrecognised
// phrase — each ends with the radio menu still being the way to order the flight, which is the #769
// decision rather than a failure.

namespace fl {

class VoiceCommandCapture final : public ISpeechToTextHandler {
  public:
    // Emitted when a transcript matched. The caller sends it exactly as it sends a menu selection.
    using CommandSink = std::function<void(ai::WingmanCommand)>;
    // Short pilot-facing status ("Listening…", "Say again?"). Empty clears it.
    using StatusSink = std::function<void(std::string)>;

    VoiceCommandCapture(IAudioCapture* capture, ISpeechToText* stt, ILogger* logger);
    ~VoiceCommandCapture() override;

    void setCommandSink(CommandSink sink) {
        m_onCommand = std::move(sink);
    }
    void setStatusSink(StatusSink sink) {
        m_onStatus = std::move(sink);
    }

    // Once per frame. `keyHeld` is the WingmanVoiceCommand action; `uiFocused` force-closes the gate
    // so a chat box cannot key the mic, exactly as VoiceChat's gate does.
    void update(bool keyHeld, bool uiFocused);

    // True when this tier can actually do anything — a capture device AND a working transcriber.
    // The HUD asks this rather than offering a prompt for a feature that cannot work.
    [[nodiscard]] bool available() const;

    [[nodiscard]] bool listening() const noexcept {
        return m_listening;
    }

    // ISpeechToTextHandler
    void onTranscript(SttRequestId id, SttStatus status, const std::string& text, const std::string& error) override;

  private:
    void startListening();
    void stopAndSubmit();
    void drainCapture();

    IAudioCapture* m_capture{nullptr};
    ISpeechToText* m_stt{nullptr};
    ILogger* m_logger{nullptr};

    CommandSink m_onCommand;
    StatusSink m_onStatus;

    bool m_listening{false};
    std::vector<int16_t> m_buffer;
    std::vector<int16_t> m_scratch;
    SttRequestId m_pending{0};
};

} // namespace fl
