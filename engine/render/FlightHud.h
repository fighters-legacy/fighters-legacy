// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/HudBuilder.h" // the one HudElement assembler (#1261)

#include "RenderTypes.h"
#include "render/IHud.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace fl {

// The HUD phosphor colour — bright military green. One definition for all four of this class's TUs
// (#1265): FlightHudCombat.cpp carried its own anonymous-namespace copy, and a HUD whose combat
// symbology is a different green from its flight symbology is a bug nobody would think to look for.
inline constexpr float kHudR = 0.0f;
inline constexpr float kHudG = 1.0f;
inline constexpr float kHudB = 0.0f;

// Static per-station facts the HUD needs to name and aim weapons (#438/#641). Resolved once when the
// client loads the aircraft's def (Game.cpp buildManualFor) and pushed via setStationInfo(); the
// dynamic facts (selected station, rounds, seeker lock) ride the snapshot on EntityRenderEntry.
struct HudStationInfo {
    std::string label;         // e.g. "AIM-9"; empty -> "STA n"
    float muzzleVelMps{0.0f};  // gun muzzle velocity for the pipper lead (#641); 0 = not a gun
    float dragDecayPerS{0.0f}; // bomb drag for the CCIP solution (#641); 0 = no drag modelled
    uint8_t kind{0};           // 0 none, 1 gun, 2 missile, 3 bomb, 4 rocket
};

// Builtin aircraft HUD (#438) — an F-14/F-16/F-18-style tactical layout built entirely from
// HudElement Text/Line/Rect: a velocity ladder (left), altitude tape (right), dual heading tapes,
// a flight-path marker + boresight cross, lower AoA/Mach/G/fuel and weapon/nav blocks, an octagonal
// combiner frame, plus the datalink radar/RWR MFD (#528/#642) and combat symbology (#641).
//
// Active only when HudFrameInput::ownship is non-null (Cockpit mode). Default colour: military green.
class FlightHud : public IHud {
  public:
    void update(const HudFrameInput& in) override;
    [[nodiscard]] std::span<const HudElement> elements() const override;

    // Per-station weapon facts (#438/#641). Set once when the client resolves the aircraft's def;
    // empty = no labels (the weapon block shows "STA n"). FlightHud-specific, deliberately not on IHud
    // — the label source is client content policy, not the HUD contract.
    void setStationInfo(std::vector<HudStationInfo> info) {
        m_stations = std::move(info);
    }

  private:
    // Per-frame draw context: the input bundle plus precomputed scalars, so the draw methods (split
    // across FlightHud.cpp / FlightHudCombat.cpp / FlightHudMfd.cpp) share one consistent derivation
    // of attitude/atmosphere instead of each re-deriving it. Defined in the header so all three
    // translation units see it.
    struct Ctx {
        const HudFrameInput& in;
        const EntityRenderEntry& e;
        float q[4];
        float altMsl;   // geodetic MSL altitude (m)
        float iasKts;   // calibrated airspeed (kt)
        float tasMps;   // true airspeed magnitude (m/s)
        float mach;     // TAS / local speed of sound
        float pitchRad; // local-level pitch
        float bankRad;  // local-level bank
        float hdgDeg;   // true heading of the nose (0 = N)
        float aoaDeg;   // angle of attack (body frame)
        float loadG;    // load factor (g)
        float aspect;   // viewport aspect for square symbology
    };

    void drawFrame(Ctx& c);         // octagon combiner outline + boresight cross + clock + latency
    void drawSpeedLadder(Ctx& c);   // left: boxed IAS (kt), 10/50-kt ticks scrolling with speed
    void drawAltTape(Ctx& c);       // right: boxed altitude (ft), 500-ft ticks
    void drawHeadingTapes(Ctx& c);  // top + bottom compass tapes, cardinal labels, lubber line
    void drawFpmAndHorizon(Ctx& c); // flight-path marker + radial artificial horizon
    void drawDataBlocks(Ctx& c);    // lower-left AoA/Mach/G/fuel, lower-right weapon/nav + autopilot
    void drawCombat(Ctx& c);        // #641 target box / pipper / CCIP (FlightHudCombat.cpp)
    void drawMfd(Ctx& c);           // #642 radar MFD + RWR pages (FlightHudMfd.cpp)

    // Appenders shared by every draw method (return false on cap overflow — see overflowed()).
    bool pushText(HudAlign align, float x, float y, float r, float g, float b, const char* fmt, ...);
    bool pushLine(float x0, float y0, float x1, float y1, float thick, float r, float g, float b, float a = 1.0f);
    bool pushRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a);

    // An 8-segment circle in HUD colour, radius corrected for aspect so it is round rather than oval
    // (#1265). Two of this class's TUs drew one — the flight-path marker and the gun pipper — and
    // each carried its own raw 3.14159265f. Eight segments is deliberate: the HUD is a stroked
    // phosphor display, and a smoother circle would cost elements from a capped arena.
    void pushCircle(float cx, float cy, float rad, float thick, float aspect);

    // Element/string storage. Caps raised for the tape/ladder tick marks and their labels (#438): the
    // classic layout drew ~15 elements; a full tactical HUD with tapes + scope is far more.
    static constexpr int kScopeMaxTracks = 40;
    static constexpr std::size_t kMaxElements = 320;
    static constexpr std::size_t kMaxStrings = 48;
    static constexpr std::size_t kFormatBytes = 64; // the HUD's labels are short by design

    HudBuilder<kMaxElements, kMaxStrings, kFormatBytes> m_hud;
    std::vector<HudStationInfo> m_stations;

  public:
    // True if the last update() overflowed the element/string caps (a silent-truncation guard for
    // tests; the worst-case frame must stay under the caps).
    [[nodiscard]] bool overflowed() const noexcept {
        return m_hud.overflowed();
    }
};

} // namespace fl
