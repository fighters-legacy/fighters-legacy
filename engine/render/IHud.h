// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "flight/Geodetic.h"  // kEarthRadiusM (default planet radius)
#include "render/RadarView.h" // RadarView (datalink scope + RWR, #528)
#include "render/RenderSnapshot.h"

#include <cstdint>
#include <functional>
#include <glm/vec3.hpp>
#include <span>

namespace fl {

// MFD-page state (#642), owned by the game layer (FlightScreen) and passed to the HUD each frame. The
// page selects which radar/RWR presentation drawMfd() renders; the default (Ppi) preserves the always-
// on datalink scope that #528 shipped.
struct HudMfdState {
    enum class Page : uint8_t { Off, Ppi, BScope, Rwr };
    Page page{Page::Ppi};
    float rangeScaleM{74080.0f}; // scope range (m); 74080 = 40 nm. Cycled 10/20/40/80 nm.
    uint8_t radarMode{1};        // sensor::RadarMode ordinal (Silent/Search/Tws/Stt) for the annunciation
};

// Everything the HUD needs for one frame (#438). A single struct instead of a growing positional-
// argument list: the combat-symbology, MFD, and autopilot consumers (#641/#642/#640) add fields here
// rather than re-churning the IHud::update signature. Every field has an inert default so a caller
// that only cares about the basic instruments can leave the rest untouched.
struct HudFrameInput {
    // Core instruments (#438).
    const EntityRenderEntry* ownship{nullptr}; // nullptr = HUD suppressed (not in Cockpit mode)
    CameraView camera{};                       // the frame's view, for world->screen symbology
    bool cameraValid{false};                   // false = skip all projected symbology (FPM, target box, pipper)
    float timeOfDay{12.0f};                    // hours [0,24) -> HH:MM clock
    float terrainElevation{0.0f};              // terrain radial elevation at ownship XZ (AGL = MSL - this)
    uint32_t latencyMs{0};                     // receiving peer one-way latency (SnapshotPeerLatency TLV)
    bool showLatency{false};                   // gate for the latency indicator
    double planetRadiusM{kEarthRadiusM};       // for the local-level attitude frame (#479)
    RadarView radar{};                         // fused datalink track picture + RWR (#528/#642)

    // Combat symbology (#641). Populated by FlightScreen from the target-designation module (#696).
    const EntityRenderEntry* designatedTarget{nullptr}; // the designated target, snapshot-lifetime (do not retain)
    bool masterArm{true};                               // client-local ARM/SAFE; gates the pipper + weapon block
    std::function<double(const glm::dvec3&)> terrainHeightAt{}; // for the CCIP fall solution; null = no CCIP cue

    // MFD radar/RWR page state (#642).
    HudMfdState mfd{};

    // Autopilot annunciation (#640). apModes is an OR of Autopilot::ModeBit; 0 = disengaged.
    uint8_t apModes{0};
    float apTargetAltM{0.0f};
    float apTargetHeadingDeg{0.0f};
    float apTargetSpeedMps{0.0f};

    // Night-vision annunciator (#210): draw an "NVG" cue when the goggles are on.
    bool nvgActive{false};
};

// Aircraft HUD interface. Displays instrument data (speed, altitude, heading, attitude, combat
// symbology, radar/RWR) in Cockpit camera mode.
//
// FlightHud is the builtin implementation. Content packs can provide a custom IHud for each aircraft
// type via the content system (future phase).
class IHud {
  public:
    virtual ~IHud() = default;

    // Build HUD elements for this frame from the bundle above. `ownship == nullptr` suppresses all
    // output (e.g. when not in Cockpit mode).
    virtual void update(const HudFrameInput& in) = 0;

    // Returns overlay elements. Valid until the next call to update().
    [[nodiscard]] virtual std::span<const HudElement> elements() const = 0;
};

} // namespace fl
