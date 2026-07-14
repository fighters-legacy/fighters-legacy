// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "audio/SfxBuiltinSounds.h"
#include "config/AudioSettings.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fl {

class AssetManager;
class ILogger;

// Positional weapon SFX (#631) — the audio counterpart to ClientEffectRouter's particles. Plays
// short one-shot clips from a preset vocabulary at a WORLD position, 3D-spatialised camera-relative.
//
// Preset resolution: a preset name resolves to a CONTENT-PACK asset when one exists (so a theater
// pack retunes the guns without touching the engine), else to a compiled-in procedural sound
// (SfxBuiltinSounds) — so the fire path has audio in the zero-pack sandbox. Full-decode buffer
// cache, one upload per unique sound (the VoiceCalloutManager pattern).
//
// 16-voice STEAL-OLDEST pool: a burst of impacts degrades to the newest 16 rather than dropping
// them silently, which is the right failure mode for cosmetics. Main-thread only.
//
// CAMERA-RELATIVE positioning (the renderer's invariant, in audio): sources are placed at
// worldPos − cameraOrigin and the listener sits at the origin, so float32 source coordinates stay
// small and precise at planet scale. Call updateListener() once per frame, then play() per event.
class SfxManager {
  public:
    // audio/assets/logger must outlive the manager. A null IAudio is tolerated (headless / CI): the
    // manager becomes a no-op, so callers need no #ifdef.
    bool init(IAudio* audio, AssetManager* assets, ILogger* logger);
    void shutdown();

    // Register a preset: a name → (pack asset name, builtin fallback). packAsset empty = builtin
    // only. Content packs override by shipping the asset; the builtin covers the zero-pack case.
    void registerPreset(const std::string& name, const std::string& packAsset, SfxKind builtin);

    // Move the listener to the camera. Sources are placed camera-relative, so the listener sits at
    // the origin looking along the camera; `forward`/`up` orient the stereo field. Call once/frame.
    void updateListener(const glm::vec3& forward, const glm::vec3& up);

    // Fire a one-shot at `worldPos`, spatialised relative to `cameraOrigin` (double-subtracted on
    // the CPU, like the renderer). `intensity` scales gain on top of the settings sliders. Unknown
    // preset = silent no-op. Distance attenuation is the HAL's (reference/rolloff set per source).
    void play(const std::string& preset, const glm::dvec3& worldPos, const glm::dvec3& cameraOrigin,
              const AudioSettings& settings, float intensity = 1.f);

    static constexpr int kMaxVoices = 16;
    static constexpr float kReferenceDistanceM = 50.f; // full gain within this radius
    static constexpr float kMaxDistanceM = 8000.f;     // audible ceiling

  private:
    struct Preset {
        std::string packAsset;
        SfxKind builtin;
    };
    AudioBufferId getOrUploadBuffer(const Preset& preset);

    IAudio* m_audio{nullptr};
    AssetManager* m_assets{nullptr};
    ILogger* m_logger{nullptr};

    std::unordered_map<std::string, Preset> m_presets;
    // Buffer cache keyed by the RESOLVED source ("pack:<asset>" or "builtin:<kind>"), so a builtin
    // and a pack override of the same preset are distinct entries.
    std::unordered_map<std::string, AudioBufferId> m_bufferCache;

    AudioSourceId m_voices[kMaxVoices]{};
    int m_nextVoice{0}; // round-robin = steal-oldest for one-shots of similar length
};

} // namespace fl
