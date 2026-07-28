// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "IAudioSynthesizer.h"
#include "config/AudioSettings.h"
#include "voice/RadioDsp.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace fl {

class AssetManager;
class ILogger;
class Localization;
class SubtitleQueue;

// A single voice callout event: audio asset + subtitle localization key.
// Either field may be nullptr to skip that component.
struct VoiceCallout {
    const char* audioAsset{nullptr};  // AssetManager::loadAudio() asset name; nullptr = no audio
    const char* subtitleKey{nullptr}; // Localization key for subtitle text; nullptr = no subtitle
    float subtitleDuration{4.0f};     // seconds the subtitle is shown
};

// Plays short OGG SFX clips and pushes localized subtitle text.
// Optionally accepts an IAudioSynthesizer to generate speech instead of
// loading a pre-recorded OGG — if synthesis fails, falls back to the OGG asset.
//
// Threading: all methods must be called from the main thread.
class VoiceCalloutManager {
  public:
    bool init(IAudio* audio, AssetManager* assets, SubtitleQueue* subtitles, Localization* i18n, ILogger* logger,
              IAudioSynthesizer* synth = nullptr);

    void play(const VoiceCallout& callout, const AudioSettings& settings);

    // Play a callout whose subtitle text is ALREADY resolved (#704) — the server sends the rendered
    // line, not a localization key. audioAsset may be null/empty (subtitle only — the no-pack-audio
    // degradation path in docs/developer/ai-architecture.md). Safe to call before init()/without an audio device.
    //
    // `radio` (#925): when non-null, the audio is put through the SAME radio treatment human voice
    // gets — band-limited and compressed by RadioFilter, bracketed with the key-down click and the
    // squelch tail. This is what makes an ATC or AWACS transmission indistinguishable from a pilot's
    // in presentation, which is the whole point of routing synthetic voice through the radio nets
    // rather than growing a parallel voicing path. Null = play it dry (a cockpit callout, not radio).
    void playText(std::string_view text, const char* audioAsset, float subtitleDuration, const AudioSettings& settings,
                  const RadioProfile* radio = nullptr, float netGain = 1.f);

    void shutdown();

  private:
    AudioBufferId getOrUploadBuffer(const char* assetName);
    AudioBufferId getOrUploadRadioBuffer(const char* assetName, const RadioProfile& profile);

    IAudio* m_audio{nullptr};
    AssetManager* m_assets{nullptr};
    SubtitleQueue* m_subtitles{nullptr};
    Localization* m_i18n{nullptr};
    ILogger* m_logger{nullptr};
    IAudioSynthesizer* m_synth{nullptr};

    // Round-robin pool of short-lived SFX sources.
    static constexpr int kMaxSfxSources = 8;
    AudioSourceId m_sources[kMaxSfxSources]{};
    int m_nextSource{0};

    // Decoded PCM buffers keyed by asset name (lazy upload, permanent cache).
    std::unordered_map<std::string, AudioBufferId> m_bufferCache;
};

} // namespace fl
