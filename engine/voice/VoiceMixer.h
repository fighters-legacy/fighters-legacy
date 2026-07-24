// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "config/AudioSettings.h"
#include "config/VoiceSettings.h"
#include "voice/RadioDsp.h"
#include "voice/RadioNet.h"
#include "voice/VoiceCodec.h"
#include "voice/VoiceJitterBuffer.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace fl {

class ILogger;

// Resolve a speaker's world position from the entity index carried on its voice frames. Returns
// false when the entity is unknown to this client (interest-culled, or the speaker is an observer
// with no aircraft) — the mixer then falls back to a head-locked mix rather than placing the voice
// at the world origin, which would put every unresolvable speaker at the north pole.
using SpeakerPositionFn = std::function<bool(uint32_t entityIdx, glm::dvec3& outWorldPos)>;

// One stream currently producing audio, for the HUD net indicator (#925).
struct ActiveSpeaker {
    uint32_t peerId{0};
    uint8_t netId{kInvalidRadioNet};
    bool positional{false};
    float level{0.f}; // smoothed RMS of the decoded audio, [0, 1]
};

// ---------------------------------------------------------------------------------------------
// Remote voice playback and spatial mix (#531)
// ---------------------------------------------------------------------------------------------
// One stream per (speaker, net) pair — NOT per speaker. A pilot transmitting on the flight net
// while ATC answers on the ATC net is two independent streams with independent codec state,
// independent de-jitter, and independent placement; collapsing them onto one source would
// interleave two conversations into one garbled channel.
//
// CAMERA-RELATIVE positioning, the same invariant the renderer and SfxManager hold: sources are
// placed at worldPos - cameraOrigin with the listener at the origin, so float32 source coordinates
// stay precise at planet scale.
//
// Head-locked is the DEFAULT and positional is opt-in per net, because that is what a radio is:
// the voice arrives in your headset, not from the other aircraft's bearing. Only the proximity net
// ships positional by default.
//
// Threading: main thread only, like every other IAudio consumer.
// ---------------------------------------------------------------------------------------------
class VoiceMixer {
  public:
    VoiceMixer();
    ~VoiceMixer();
    VoiceMixer(const VoiceMixer&) = delete;
    VoiceMixer& operator=(const VoiceMixer&) = delete;

    // A null IAudio is tolerated (headless / CI / no device): every method becomes a cheap no-op,
    // so callers need no #ifdef and no null checks of their own.
    bool init(IAudio* audio, ILogger* logger);
    void shutdown();

    void setNets(const RadioNetTable& nets);
    void applySettings(const VoiceSettings& voice, const AudioSettings& audio);

    // Feed one received frame. An empty payload with `end` set is a pure end-of-transmission
    // marker (the squelch cue, #925) and carries no audio.
    void onFrame(uint32_t senderPeerId, uint32_t senderEntityIdx, uint8_t netId, uint16_t seq,
                 std::span<const uint8_t> payload, bool start, bool end);

    // Once per rendered frame: decode + queue audio, place sources, retire finished streams.
    void update(float dt, const glm::dvec3& cameraOrigin, const SpeakerPositionFn& posFn);

    // Drop every stream (session end, disconnect). Safe to call repeatedly.
    void reset();

    [[nodiscard]] std::span<const ActiveSpeaker> activeSpeakers() const noexcept {
        return m_active;
    }
    [[nodiscard]] bool anyActive() const noexcept {
        return !m_active.empty();
    }

    // Hold the ducking envelope open for `seconds` even though no relayed frame is arriving. This is
    // how SYNTHETIC radio traffic (ATC, AWACS, Epic O TTS — played through VoiceCalloutManager, not
    // through the mixer) ducks the music exactly like a human transmission does. Without it, the one
    // audible difference between human and synthetic radio would be that only one of them makes the
    // music get out of the way.
    void holdDuck(float seconds) noexcept;

    // 1 = no ducking; < 1 while a net is live (#925). Smoothed, so music does not pump on every
    // syllable — it dips once when the net opens and recovers over ~0.4 s after it closes.
    [[nodiscard]] float duckGain() const noexcept {
        return m_duckGain;
    }

    // Streaming buffers per voice source. 4 x 20 ms = 80 ms of queued audio: enough that a frame
    // spike between renders does not starve OpenAL, short enough to keep the call conversational.
    static constexpr int kStreamBuffers = 4;
    static constexpr int kTargetQueued = 3;
    // Retire a stream this many seconds after its last audio. Long enough to survive a gap between
    // two transmissions from the same speaker (so the decoder/source is not rebuilt per sentence),
    // short enough that 128 players do not accumulate idle sources.
    static constexpr float kStreamIdleTimeoutS = 2.0f;
    static constexpr float kDuckAttackPerSec = 8.0f;  // fast dip when a net opens
    static constexpr float kDuckReleasePerSec = 2.5f; // gentle recovery when it closes
    // Positional-net distance model. Full gain out to 500 m (formation spacing), then rolling off;
    // a net that declares its own rangeM overrides the ceiling.
    static constexpr float kPositionalReferenceM = 500.f;
    static constexpr float kPositionalMaxM = 8000.f;

  private:
    struct Stream {
        uint32_t peerId{0};
        uint32_t entityIdx{0xFFFFFFFFu};
        uint8_t netId{kInvalidRadioNet};
        std::unique_ptr<VoiceDecoder> decoder;
        VoiceJitterBuffer jitter;
        // Per-stream, because filter memory shared across speakers would smear one voice's tail
        // into the next (#925).
        RadioFilter filter;
        AudioSourceId source{0};
        AudioBufferId buffers[kStreamBuffers]{};
        int queued{0};    // buffers currently queued on the source (we track; AL only reports processed)
        int freeCount{0}; // free-list size
        AudioBufferId freeList[kStreamBuffers]{};
        float idleSec{0.f}; // seconds since the last decoded frame
        float level{0.f};   // smoothed RMS for the HUD
        bool started{false};
        bool ending{false};
        bool pendingClick{false}; // queue the key-down click before the first decoded frame (#925)
        bool tailQueued{false};   // the squelch tail has been queued; retire once it drains
    };

    Stream* findStream(uint32_t peerId, uint8_t netId);
    Stream* createStream(uint32_t peerId, uint8_t netId);
    void destroyStream(Stream& s);
    void pumpStream(Stream& s, float dt);
    void placeStream(Stream& s, const glm::dvec3& cameraOrigin, const SpeakerPositionFn& posFn);
    [[nodiscard]] float netVolume(uint8_t netId) const;

    IAudio* m_audio{nullptr};
    ILogger* m_logger{nullptr};
    RadioNetTable m_nets;
    VoiceSettings m_voice;
    AudioSettings m_audioSettings;
    // unique_ptr elements so a Stream* handed out by findStream/createStream stays valid when the
    // list grows mid-update (and so Stream needs no move semantics of its own).
    std::vector<std::unique_ptr<Stream>> m_streams;
    std::vector<ActiveSpeaker> m_active;
    std::vector<int16_t> m_pcm;     // scratch decode buffer, kVoiceFrameSamples
    std::vector<uint8_t> m_payload; // scratch, popped from the jitter buffer
    // Procedural radio cues, generated once (deterministic + byte-stable, so they are identical on
    // every machine) and reused for every transmission on every net.
    std::vector<int16_t> m_clickPcm;
    std::vector<int16_t> m_squelchPcm;
    float m_duckGain{1.f};
    float m_duckHoldSec{0.f}; // synthetic-transmission hold (see holdDuck)
};

} // namespace fl
