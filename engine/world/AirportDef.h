// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// What a runway is paved with. This is an AUTHORING vocabulary — what the surface IS made of —
// distinct from engine/render/SurfaceType.h's WorldCover land-cover vocabulary (Grass/Forest/Water/
// …). They are deliberately two enums: a runway is never "forest", and a land-cover class is never
// "deck". #487 adds a total RunwaySurface -> SurfaceType bridge (engine/render/RunwaySurfaceMap.h)
// so terrain queries can report the runway surface, without either enum leaking the other's values.
//
// Deck = a carrier/ship flight deck; landing logic keys on AirportDef::acceptsLandings, so a carrier
// reuses the same recovery path as a land airfield (the FA design lesson, #673).
enum class RunwaySurface : uint8_t {
    Concrete,
    Asphalt,
    Grass,
    Gravel,
    Water,
    Deck,
};

// Lowercase canonical name for a surface (round-trips with runwaySurfaceFromString).
[[nodiscard]] constexpr std::string_view runwaySurfaceName(RunwaySurface s) noexcept {
    switch (s) {
    case RunwaySurface::Concrete:
        return "concrete";
    case RunwaySurface::Asphalt:
        return "asphalt";
    case RunwaySurface::Grass:
        return "grass";
    case RunwaySurface::Gravel:
        return "gravel";
    case RunwaySurface::Water:
        return "water";
    case RunwaySurface::Deck:
        return "deck";
    }
    return "asphalt";
}

// Parse a surface name (case-sensitive lowercase, as written in TOML). Returns the enum, or nullopt
// for an unknown string so the caller can reject it — an author who mistypes "ashpalt" should hear
// about it, not silently get asphalt.
[[nodiscard]] constexpr bool runwaySurfaceFromString(std::string_view s, RunwaySurface& out) noexcept {
    if (s == "concrete") {
        out = RunwaySurface::Concrete;
        return true;
    }
    if (s == "asphalt") {
        out = RunwaySurface::Asphalt;
        return true;
    }
    if (s == "grass") {
        out = RunwaySurface::Grass;
        return true;
    }
    if (s == "gravel") {
        out = RunwaySurface::Gravel;
        return true;
    }
    if (s == "water") {
        out = RunwaySurface::Water;
        return true;
    }
    if (s == "deck") {
        out = RunwaySurface::Deck;
        return true;
    }
    return false;
}

// One runway. headingDeg is the TRUE bearing of the centerline, threshold -> far end (0 = north,
// 90 = east). A physical runway is bidirectional; this describes it from one end (the reciprocal is
// heading + 180). lengthM/widthM are the paved dimensions.
struct RunwayDef {
    float headingDeg{90.f};
    float lengthM{2500.f};
    float widthM{45.f};
    RunwaySurface surface{RunwaySurface::Asphalt};
};

// Immutable definition of one airport/airfield/carrier deck. Placed on the spherical Earth by
// AirportRegistry, which resolves the world position and per-runway geometry once at load.
//
// Placement is EITHER geodetic (latRad/lonRad) OR direct world-XZ (useWorldXZ + worldX/worldZ). The
// world-XZ form exists for a field placed beside the world ORIGIN, which is the NORTH POLE in engine
// coordinates (worldToGeodetic(0,0,0) -> lat = pi/2), where longitude and the ENU basis are singular
// and lat/lon cannot express a nearby point usefully.
//
// The builtin sandbox strip no longer needs it: the sandbox home moved off the pole (#1211,
// world/SandboxHome.h) and builtinAirfield() is geodetic like any other airport. Prefer lat/lon.
struct AirportDef {
    std::string id;   // namespaced def id ("fl-base:homeplate", "builtin:airfield", or an ICAO)
    std::string name; // human-readable
    double latRad{0.0};
    double lonRad{0.0};
    bool useWorldXZ{false}; // true: place at (worldX, ·, worldZ); false: place at (latRad, lonRad)
    double worldX{0.0};
    double worldZ{0.0};
    double elevationM{-1.0}; // authoritative field elevation above the datum; < 0 = resolve from terrain
    bool acceptsLandings{true};
    std::string factionId; // empty = neutral
    std::vector<RunwayDef> runways;
};

} // namespace fl
