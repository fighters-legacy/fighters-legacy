// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "audio/EngineAudio.h" // engineTone / windRush* / generateEngineLoopPcm (engine-audio)
#include "config/AudioSettings.h"
#include "render/RenderSnapshot.h" // EntityRenderEntry

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace fl {

class ILogger;

// Continuous engine + aerodynamic sound layers (#959, part of Epic #586) — the flying counterpart to
// ClientEffectRouter's one-shot particles, and a game-layer presentation router for the same reason:
// it consumes the render snapshot (EntityRenderEntry) and owns OpenAL sources, so it lives beside the
// other snapshot→presentation routers rather than in engine-audio (which stays render-free). The pure
// DSP + throttle/airspeed→pitch/gain mapping it drives lives in engine/audio/EngineAudio.
//
// Two kinds of layer:
//   * OWN-SHIP: a HEAD-LOCKED engine loop (setSourceRelative + rolloff 0) whose pitch/gain track the
//     own aircraft's throttle/airspeed/afterburner, plus a wind RUSH layer swelling with dynamic
//     pressure. Its source velocity is pinned to the listener's, so there is no doppler on your own
//     engine.
//   * OTHER AIRCRAFT: a small pool of POSITIONAL looping engine sources, one per nearby air entity,
//     placed camera-relative with the entity's world velocity set so OpenAL applies doppler on a
//     flyby. Distance attenuation is the HAL's (reference/max distance + rolloff).
//
// A null IAudio is tolerated (headless / CI): every method is a no-op, so callers need no #ifdef.
// Main-thread only; call once per frame AFTER the listener transform/velocity are set.
class EngineAudioManager {
  public:
    // A predicate deciding whether a snapshot entity is an AIR VEHICLE (only those get an engine
    // layer — a parked crate or a projectile does not hum). Injected so this needs no
    // EntityTypeRegistry dep at construction; the game supplies it from the registry's category.
    // Unset ⇒ every non-own entity qualifies (fine for the all-aircraft sandbox).
    using AirVehiclePredicate = std::function<bool(uint32_t typeIndex)>;

    // audio/logger must outlive the manager. Sources/buffers are created lazily on first use.
    bool init(IAudio* audio, ILogger* logger);
    void shutdown();

    void setAirVehiclePredicate(AirVehiclePredicate pred) {
        m_isAir = std::move(pred);
    }

    // Drive the layers one frame from the current render snapshot. `ownIdx` is the listener's own
    // entity index (its engine is head-locked); EngineAudioManager::kNoEntity = no own entity
    // (observer / menu) — the own-ship layers go silent, positional flyby layers still play. Sources
    // are placed camera-relative (listener at origin), so `cameraOrigin` matches what the listener
    // transform was set from this frame. gain follows master*sfx live.
    void update(std::span<const EntityRenderEntry> entities, uint32_t ownIdx, const glm::dvec3& cameraOrigin,
                const AudioSettings& settings);

    // Number of positional flyby voices currently sounding (test / telemetry).
    [[nodiscard]] int activeFlybyVoices() const noexcept {
        return m_activeFlyby;
    }

    static constexpr int kMaxFlybyVoices = 8;                // nearest-N other aircraft get a source
    static constexpr float kFlybyReferenceDistanceM = 150.f; // full gain within this radius
    static constexpr float kFlybyMaxDistanceM = 7000.f;      // audible ceiling
    static constexpr uint32_t kNoEntity = 0xFFFFFFFFu;

  private:
    AudioBufferId engineBuffer();
    AudioBufferId windBuffer();
    void ensureOwnSources();
    void silenceOwn();
    void driveOwn(const EntityRenderEntry& own, const AudioSettings& settings);
    void driveFlybys(std::span<const EntityRenderEntry> entities, uint32_t ownIdx, const glm::dvec3& cameraOrigin,
                     const AudioSettings& settings);

    struct FlybyVoice {
        AudioSourceId src{0};
        uint32_t entityIdx{kNoEntity}; // entity this voice currently tracks (kNoEntity = free)
        bool playing{false};
    };

    IAudio* m_audio{nullptr};
    ILogger* m_logger{nullptr};
    AirVehiclePredicate m_isAir;

    AudioBufferId m_engineBuf{0};
    AudioBufferId m_windBuf{0};

    AudioSourceId m_ownEngine{0};
    AudioSourceId m_ownWind{0};
    bool m_ownPlaying{false};

    FlybyVoice m_flyby[kMaxFlybyVoices]{};
    int m_activeFlyby{0};
};

} // namespace fl
