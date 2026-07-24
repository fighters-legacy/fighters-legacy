// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudioCapture.h"
#include "voice/VoiceActivity.h"
#include "voice/VoiceCodec.h"
#include "voice/VoiceMixer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fl {

class ILogger;

// Where an encoded local frame goes. The client wires this to a MsgVoiceFrame send (#532); tests
// and tools wire it to a recorder. VoiceChat itself knows nothing about the network — the same
// std::function-across-the-CMake-boundary seam the rest of the engine uses.
using VoiceFrameSink =
    std::function<void(uint8_t netId, uint16_t seq, std::span<const uint8_t> payload, bool start, bool end)>;

// ---------------------------------------------------------------------------------------------
// The client voice facade (#531)
// ---------------------------------------------------------------------------------------------
// Owns the whole local half of a radio: capture device, keying gate, encoder, net selection — and
// the receive-side VoiceMixer, so the game layer holds ONE object with one update() rather than
// five that must be stepped in the right order.
//
// Everything degrades softly and independently: no capture device is listen-only, no encoder is
// listen-only, a null IAudio is send-only, and voice disabled entirely is a no-op. None of these
// is an error the player has to resolve before flying.
//
// Threading: main thread only.
// ---------------------------------------------------------------------------------------------
class VoiceChat {
  public:
    VoiceChat();
    ~VoiceChat();
    VoiceChat(const VoiceChat&) = delete;
    VoiceChat& operator=(const VoiceChat&) = delete;

    // `capture` and `audio` may both be null (headless / no device); init still succeeds and the
    // corresponding half is simply inert. Returns true when at least one half is live.
    bool init(IAudioCapture* capture, IAudio* audio, ILogger* logger);
    void shutdown();

    // Replace the net table (server-authoritative, arrives after ConnectAck) and re-derive the PTT
    // net selection. Clears nothing on the receive side: streams key on netId, which is stable for
    // the life of a session.
    void setNets(const RadioNetTable& nets);
    [[nodiscard]] const RadioNetTable& nets() const noexcept {
        return m_nets;
    }

    void applySettings(const VoiceSettings& voice, const AudioSettings& audio);
    [[nodiscard]] const VoiceSettings& settings() const noexcept {
        return m_voice;
    }

    void setFrameSink(VoiceFrameSink sink) {
        m_sink = std::move(sink);
    }

    // Once per rendered frame. `pttPrimary`/`pttSecondary` are the two PTT key states; the
    // secondary key wins when both are held (a pilot mashing both means the urgent one).
    // `uiFocused` force-closes the gate — a chat box or a menu must never transmit keystrokes'
    // worth of room noise.
    void update(float dt, bool pttPrimary, bool pttSecondary, bool uiFocused, const glm::dvec3& cameraOrigin,
                const SpeakerPositionFn& posFn);

    // A frame received from the network, handed straight to the mixer.
    void onRemoteFrame(uint32_t senderPeerId, uint32_t senderEntityIdx, uint8_t netId, uint16_t seq,
                       std::span<const uint8_t> payload, bool start, bool end);

    // Tell the encoder what the link is actually doing, so Opus spends FEC bits in proportion.
    void setExpectedPacketLoss(int percent);

    // Point the PRIMARY key at the next net in the table (the VoiceNetCycle action).
    void cyclePrimaryNet();
    [[nodiscard]] uint8_t primaryNet() const noexcept {
        return m_primaryNet;
    }
    [[nodiscard]] uint8_t secondaryNet() const noexcept {
        return m_secondaryNet;
    }
    // Display label for a net index ("TEAM"), or an empty view for an unknown index.
    [[nodiscard]] std::string_view netName(uint8_t netId) const noexcept;

    // True while the local mic is keyed. Drives the HUD "TX" indicator.
    [[nodiscard]] bool transmitting() const noexcept {
        return m_gate.isOpen();
    }
    [[nodiscard]] uint8_t transmittingNet() const noexcept {
        return m_txNet;
    }
    // Smoothed input level in [0, 1] — the settings-screen mic meter and the HUD TX bar.
    [[nodiscard]] float micLevel() const noexcept {
        return m_micLevel;
    }
    [[nodiscard]] bool captureAvailable() const noexcept {
        return m_captureReady;
    }
    [[nodiscard]] const std::string& captureError() const noexcept {
        return m_captureError;
    }

    [[nodiscard]] VoiceMixer& mixer() noexcept {
        return m_mixer;
    }
    [[nodiscard]] const VoiceMixer& mixer() const noexcept {
        return m_mixer;
    }

    // Session teardown (disconnect / leaving the flight screen): closes the gate, emits the
    // end-of-transmission marker if one was open, and drops every receive stream.
    void reset();

  private:
    void ensureCapture();
    void closeCapture();
    void processFrame(std::span<const int16_t> pcm, bool keyHeld);
    void deriveNetSelection();

    IAudioCapture* m_capture{nullptr};
    ILogger* m_logger{nullptr};
    std::unique_ptr<VoiceEncoder> m_encoder;
    VoiceMixer m_mixer;
    VoiceActivityGate m_gate;
    RadioNetTable m_nets;
    VoiceSettings m_voice;
    AudioSettings m_audioSettings;
    VoiceFrameSink m_sink;

    std::vector<int16_t> m_captureBuf; // drain scratch
    std::vector<int16_t> m_accum;      // partial frame carried across updates
    std::vector<uint8_t> m_encoded;    // kMaxVoicePayloadBytes

    uint8_t m_primaryNet{kInvalidRadioNet};
    uint8_t m_secondaryNet{kInvalidRadioNet};
    uint8_t m_txNet{kInvalidRadioNet}; // the net the CURRENT transmission opened on; held until it closes
    uint16_t m_seq{0};
    float m_micLevel{0.f};
    bool m_captureReady{false};
    bool m_captureAttempted{false};
    std::string m_captureError;
};

} // namespace fl
