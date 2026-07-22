// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlightHud redesign tests (#438): the F-14-style tactical layout driven by the HudFrameInput bundle
// (velocity ladder, altitude tape, heading tapes, flight-path marker + radial horizon, AoA/Mach/G/fuel
// and weapon blocks). Semantic checks over the emitted HudElement list — no GPU.

#include "flight/Geodetic.h" // kEarthRadiusM
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/RadarView.h"
#include "render/RenderSnapshot.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>
#include <vector>

using namespace fl;

namespace {

EntityRenderEntry makeEntry() {
    EntityRenderEntry e;
    e.entityIdx = 1;
    e.entityGen = 1;
    e.position = {0.0, 3500.0, 0.0};
    e.velocity = {0.f, 0.f, 0.f};
    e.orientation = glm::quat(1.f, 0.f, 0.f, 0.f); // identity
    e.omega = {0.f, 0.f, 0.f};
    e.damageLevel = 0;
    e.playerOwned = true;
    e.throttle = 0;
    e.fuelPct = 0;
    return e;
}

// A camera looking along -Z from the entity (so the FPM has a valid projection when moving).
CameraView camAt(glm::dvec3 eye) {
    CameraController cc;
    cc.setPose(eye, glm::vec3{0.f, 0.f, -1.f}, glm::vec3{0.f, 1.f, 0.f});
    return cc.view(16.f / 9.f);
}

HudFrameInput makeInput(const EntityRenderEntry& e, float tod = 12.f, float terrain = 0.f) {
    HudFrameInput in;
    in.ownship = &e;
    in.camera = camAt(e.position);
    in.cameraValid = true;
    in.timeOfDay = tod;
    in.terrainElevation = terrain;
    return in;
}

bool hasText(const FlightHud& hud, const char* needle) {
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find(needle) != std::string_view::npos)
            return true;
    return false;
}

// The artificial horizon is the WIDEST near-horizontal Line near screen centre (the octagon frame and
// boresight/tape lines are all shorter). Returns the widest such line.
const HudElement* findHorizon(const FlightHud& hud) {
    const HudElement* best = nullptr;
    float bestSpan = 0.05f;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Line)
            continue;
        const float midY = 0.5f * (el.y + el.y2);
        if (midY < 0.24f || midY > 0.76f)
            continue;
        const float span = std::abs(el.x2 - el.x);
        if (span > bestSpan) {
            bestSpan = span;
            best = &el;
        }
    }
    return best;
}

} // namespace

TEST_CASE("FlightHud: null ownship produces no elements (#438)", "[flight_hud]") {
    FlightHud hud;
    HudFrameInput in; // ownship nullptr
    hud.update(in);
    CHECK(hud.elements().empty());
}

TEST_CASE("FlightHud: a valid entry produces elements and does not overflow the caps (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    hud.update(makeInput(e));
    CHECK(hud.elements().size() > 0);
    CHECK_FALSE(hud.overflowed());
}

TEST_CASE("FlightHud: worst-case frame (full radar + weapons) stays under the caps (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.hasLoadout = true;
    e.selectedStation = 0;
    e.stationRounds = 200;
    e.weaponFlags = 0x01;
    e.damageLevel = 2;

    // A radar view packed to the limits.
    std::vector<RadarTrack> tracks(48);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        tracks[i].pos[0] = 100.0 * static_cast<double>(i);
        tracks[i].pos[1] = 3500.0;
        tracks[i].pos[2] = 500.0;
        tracks[i].ident = static_cast<uint8_t>(i % 3);
        tracks[i].firingQuality = (i % 4 == 0);
    }
    std::vector<RwrStrobe> strobes(16);
    for (std::size_t i = 0; i < strobes.size(); ++i) {
        strobes[i].emitterPos[0] = -200.0 * static_cast<double>(i);
        strobes[i].emitterPos[1] = 3500.0;
        strobes[i].emitterPos[2] = -400.0;
        strobes[i].level = static_cast<uint8_t>(i % 3);
    }
    RadarView radar;
    radar.tracks = std::span<const RadarTrack>(tracks.data(), tracks.size());
    radar.strobes = std::span<const RwrStrobe>(strobes.data(), strobes.size());
    radar.valid = true;

    auto in = makeInput(e, 9.f);
    in.radar = radar;
    in.latencyMs = 88;
    in.showLatency = true;
    in.apModes = 0x7; // all three autopilot holds annunciated
    hud.update(in);
    CHECK_FALSE(hud.overflowed());
}

TEST_CASE("FlightHud: velocity ladder shows calibrated IAS in knots (#438/#480)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, 0.0, 0.0};   // sea level: IAS == TAS
    e.velocity = {0.f, 0.f, 100.f}; // 100 m/s -> ~194 kt
    hud.update(makeInput(e));
    CHECK(hasText(hud, "IAS")); // the ladder tag
    CHECK(hasText(hud, "194")); // the boxed readout
}

TEST_CASE("FlightHud: altitude tape shows MSL in feet and an AGL readout (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.position.y = 3500.0;                  // 3500 m ~= 11483 ft
    hud.update(makeInput(e, 12.f, 1000.f)); // terrain 1000 m -> AGL 2500 m ~= 8202 ft
    CHECK(hasText(hud, "ALT"));
    CHECK(hasText(hud, "11483")); // MSL in feet
    CHECK(hasText(hud, "AGL"));
    CHECK(hasText(hud, "8202")); // AGL in feet
}

TEST_CASE("FlightHud: heading tape shows HDG and a cardinal label (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry(); // identity -> heading 0 (North)
    hud.update(makeInput(e));
    CHECK(hasText(hud, "HDG"));
    // A cardinal label appears on the tape (identity heading lands on a cardinal). Sweep yaw across a
    // full turn to be independent of the exact identity->heading mapping: North must appear somewhere.
    bool northSeen = false;
    for (int deg = 0; deg < 360 && !northSeen; deg += 15) {
        auto ey = makeEntry();
        ey.orientation = glm::angleAxis(glm::radians(static_cast<float>(deg)), glm::vec3(0.f, 1.f, 0.f));
        hud.update(makeInput(ey));
        for (const auto& el : hud.elements())
            if (el.type == HudElement::Type::Text && el.text == "N")
                northSeen = true;
    }
    CHECK(northSeen);
}

TEST_CASE("FlightHud: lower-left block shows Mach, G and fuel; weapon block shows ARM (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, 8000.0, 0.0};
    e.velocity = {0.f, 0.f, 250.f};
    e.throttle = 85;
    e.fuelPct = 60;
    hud.update(makeInput(e));
    CHECK(hasText(hud, "M "));   // Mach readout
    CHECK(hasText(hud, "G "));   // load factor
    CHECK(hasText(hud, "FUEL")); // fuel
    CHECK(hasText(hud, "THR"));  // throttle
    CHECK(hasText(hud, "85"));   // throttle value
    CHECK(hasText(hud, "ARM"));  // master arm default
}

TEST_CASE("FlightHud: text is bright military green, damage warning is red (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.damageLevel = 1;
    hud.update(makeInput(e));
    // A green instrument text exists.
    bool green = false, redDamage = false;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        if (el.text.find("DAMAGE") != std::string_view::npos) {
            redDamage = (el.r > 0.9f && el.g < 0.5f && el.align == HudAlign::Center);
        } else if (el.r < 0.1f && el.g > 0.9f && el.b < 0.1f) {
            green = true;
        }
    }
    CHECK(green);
    CHECK(redDamage);
}

TEST_CASE("FlightHud: clock is right-aligned; latency shown only when enabled (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    auto in = makeInput(e, 9.f);
    in.latencyMs = 120;
    in.showLatency = true;
    hud.update(in);
    bool clock = false, latency = false;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        if (el.text.find("09:00") != std::string_view::npos) {
            clock = true;
            CHECK(el.align == HudAlign::Right);
        }
        if (el.text.find("120") != std::string_view::npos && el.text.find("ms") != std::string_view::npos)
            latency = true;
    }
    CHECK(clock);
    CHECK(latency);

    // With showLatency = false, no latency indicator.
    auto in2 = makeInput(e);
    in2.latencyMs = 120;
    in2.showLatency = false;
    hud.update(in2);
    CHECK_FALSE(hasText(hud, "ms"));
}

// ── attitude (radial artificial horizon, #479) ───────────────────────────────

TEST_CASE("FlightHud: level flight puts the horizon through screen centre (#438/#479)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    hud.update(makeInput(e));
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    CHECK(0.5f * (h->y + h->y2) == Catch::Approx(0.5f).margin(2e-3f));
    CHECK(h->y == Catch::Approx(h->y2).margin(2e-3f)); // wings level -> untilted
}

TEST_CASE("FlightHud: nose-up drops the horizon below centre (#438/#479)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.orientation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 0.f, 1.f)); // nose up
    hud.update(makeInput(e));
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    CHECK(0.5f * (h->y + h->y2) > 0.5f);
}

TEST_CASE("FlightHud: bank tilts the horizon line (#438/#479)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.orientation = glm::angleAxis(glm::radians(30.f), glm::vec3(1.f, 0.f, 0.f)); // roll
    hud.update(makeInput(e));
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    CHECK(std::abs(h->y - h->y2) > 1e-3f);
}

TEST_CASE("FlightHud: attitude stays correct far from the world origin (#438/#479)", "[flight_hud]") {
    constexpr double R = kEarthRadiusM;
    FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, -R, R};                                                    // equator; local up = +Z
    e.orientation = glm::angleAxis(glm::radians(90.f), glm::vec3(1.f, 0.f, 0.f)); // wings level on local up
    auto in = makeInput(e);
    in.camera = camAt(e.position);
    in.planetRadiusM = R;
    hud.update(in);
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    CHECK(0.5f * (h->y + h->y2) == Catch::Approx(0.5f).margin(3e-3f));
    CHECK(h->y == Catch::Approx(h->y2).margin(3e-3f));
}

// ── flight-path marker, AoA, G ───────────────────────────────────────────────

TEST_CASE("FlightHud: the flight-path marker sits below centre in a nose-down descent (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    // Level attitude but the velocity vector points down-and-forward (descending flight path).
    e.velocity = {0.f, -40.f, -100.f}; // moving forward (-Z) and down (-Y)
    auto in = makeInput(e);
    hud.update(in);
    // The FPM circle segments cluster below centre. Find any Line whose midpoint is below 0.5 near x=0.5
    // that is NOT the horizon (short span).
    bool fpmBelow = false;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Line)
            continue;
        const float mx = 0.5f * (el.x + el.x2), my = 0.5f * (el.y + el.y2);
        const float span = std::abs(el.x2 - el.x);
        if (span < 0.05f && std::abs(mx - 0.5f) < 0.1f && my > 0.52f)
            fpmBelow = true;
    }
    CHECK(fpmBelow);
}

TEST_CASE("FlightHud: G reads ~1 in wings-level flight with no rotation (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    e.velocity = {0.f, 0.f, 100.f};
    e.omega = {0.f, 0.f, 0.f};
    hud.update(makeInput(e));
    // Find the "G  N.N" readout and parse it.
    bool found = false;
    for (const auto& el : hud.elements()) {
        if (el.type == HudElement::Type::Text && el.text.rfind("G ", 0) == 0) {
            const float g = std::atof(el.text.data() + 2);
            CHECK(g == Catch::Approx(1.0f).margin(0.05f));
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("FlightHud: AoA is positive when the relative wind comes from below the nose (#438)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    // Nose level (identity), velocity forward but with a downward component in the world = wind from
    // below in the body frame -> positive AoA.
    e.velocity = {0.f, -20.f, -100.f};
    hud.update(makeInput(e));
    bool found = false;
    for (const auto& el : hud.elements()) {
        if (el.type == HudElement::Type::Text && el.text.rfind("a ", 0) == 0) {
            const float aoa = std::atof(el.text.data() + 2);
            CHECK(aoa > 0.f);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("FlightHud: satisfies IHud via an abstract pointer (#438)", "[flight_hud]") {
    FlightHud concrete;
    IHud* hud = &concrete;
    HudFrameInput in;
    hud->update(in);
    CHECK(hud->elements().empty());
    auto e = makeEntry();
    hud->update(makeInput(e));
    CHECK_FALSE(hud->elements().empty());
}

// ── MFD radar scope (#528 relocated into drawMfd) ────────────────────────────

TEST_CASE("FlightHud: the datalink MFD draws contacts and an RWR launch caption (#438/#642)", "[flight_hud]") {
    FlightHud hud;
    auto e = makeEntry();
    RadarTrack t{};
    t.pos[0] = 0.0;
    t.pos[1] = 3500.0;
    t.pos[2] = -5000.0; // 5 km ahead
    t.ident = kIffFoe;
    RwrStrobe s{};
    s.emitterPos[0] = 0.0;
    s.emitterPos[1] = 3500.0;
    s.emitterPos[2] = -5000.0;
    s.level = kThreatLaunch;
    RadarView radar;
    radar.tracks = std::span<const RadarTrack>(&t, 1);
    radar.strobes = std::span<const RwrStrobe>(&s, 1);
    radar.valid = true;
    auto in = makeInput(e);
    in.radar = radar;
    hud.update(in);
    // A foe contact -> a red Rect blip; and the launch caption.
    bool redBlip = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Rect && el.r > 0.9f && el.g < 0.5f)
            redBlip = true;
    CHECK(redBlip);
    CHECK(hasText(hud, "LAUNCH"));

    // Page Off suppresses the scope but keeps the launch caption... which lives in drawMfd, so with the
    // whole MFD off there is no caption either. Verify the scope disappears when the page is Off.
    in.mfd.page = HudMfdState::Page::Off;
    hud.update(in);
    bool anyBlip = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Rect && el.r > 0.9f && el.g < 0.5f && el.a > 0.5f)
            anyBlip = true;
    CHECK_FALSE(anyBlip);
}

// ── combat symbology (#641) ──────────────────────────────────────────────────

TEST_CASE("FlightHud: the gun pipper appears with a gun station + designated target + master arm (#641)",
          "[flight_hud]") {
    FlightHud hud;
    HudStationInfo gun;
    gun.label = "M61";
    gun.kind = 1; // gun
    gun.muzzleVelMps = 1000.f;
    hud.setStationInfo({gun});

    auto own = makeEntry();
    own.position = {0.0, 3500.0, 0.0};
    own.velocity = {0.f, 0.f, -200.f}; // flying along -Z toward the target
    own.hasLoadout = true;
    own.selectedStation = 0;
    own.stationRounds = 500;

    auto target = makeEntry();
    target.entityIdx = 2;
    target.position = {0.0, 3500.0, -1500.0}; // 1.5 km dead ahead
    target.velocity = {80.f, 0.f, 0.f};       // crossing

    auto in = makeInput(own);
    in.designatedTarget = &target;
    in.masterArm = true;

    hud.update(in);
    // The pipper is a cluster of Lines near the projected lead point (small span, near screen centre-ish).
    int pipLines = 0;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Line && std::abs(el.x2 - el.x) < 0.03f && el.y > 0.3f && el.y < 0.7f)
            ++pipLines;
    CHECK(pipLines >= 6); // 8-seg circle + dot

    // SAFE suppresses the pipper.
    in.masterArm = false;
    hud.update(in);
    int pipLines2 = 0;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Line && std::abs(el.x2 - el.x) < 0.03f && el.y > 0.45f && el.y < 0.55f)
            ++pipLines2;
    CHECK(pipLines2 < pipLines);

    // The SAFE flag shows in the weapon block.
    CHECK(hasText(hud, "SAFE"));
}

TEST_CASE("FlightHud: the weapon-status block lists all stations, selected bracketed (#641)", "[flight_hud]") {
    FlightHud hud;
    HudStationInfo a;
    a.label = "AIM9";
    a.kind = 2;
    HudStationInfo b;
    b.label = "AIM9";
    b.kind = 2;
    hud.setStationInfo({a, b});

    auto own = makeEntry();
    own.hasLoadout = true;
    own.selectedStation = 1;
    own.stationRounds = 2;
    auto in = makeInput(own);
    hud.update(in);
    // Selected station 2 is bracketed "[2]".
    CHECK(hasText(hud, "[2]"));
    CHECK(hasText(hud, "AIM9"));
}
