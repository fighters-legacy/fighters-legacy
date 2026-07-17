// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Geodetic.h" // kEarthRadiusM
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/RenderSnapshot.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace Catch::Matchers;
using namespace fl;

// Build a minimal valid EntityRenderEntry for testing.
static fl::EntityRenderEntry makeEntry() {
    fl::EntityRenderEntry e;
    e.entityIdx = 1;
    e.entityGen = 1;
    e.position = {0.0, 3500.0, 0.0};
    e.velocity = {0.f, 0.f, 0.f};
    e.orientation = glm::quat(1.f, 0.f, 0.f, 0.f); // identity
    e.damageLevel = 0;
    e.playerOwned = true;
    e.throttle = 0;
    e.fuelPct = 0;
    return e;
}

TEST_CASE("FlightHud update with null entry produces no elements") {
    fl::FlightHud hud;
    hud.update(nullptr);
    CHECK(hud.elements().empty());
}

TEST_CASE("FlightHud produces elements for valid entry") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e);
    CHECK(hud.elements().size() > 0);
}

TEST_CASE("FlightHud shows calibrated IAS, not raw groundspeed (#480)") {
    // At sea level in still air, IAS == TAS, so 100 m/s reads ~194 kts.
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, 0.0, 0.0}; // sea level
    e.velocity = {0.f, 0.f, 100.f};
    hud.update(&e);
    bool foundIas = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("IAS") != std::string_view::npos &&
            el.text.find("194") != std::string_view::npos)
            foundIas = true;
    CHECK(foundIas);
}

TEST_CASE("FlightHud IAS falls below groundspeed at altitude; shows Mach (#480)") {
    // Same 100 m/s TAS at 10 km reads a much lower IAS (density lapse) — the whole point of the
    // distinction the old raw-magnitude "IAS" hid — and a Mach readout appears.
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, 10000.0, 0.0};
    e.velocity = {0.f, 0.f, 100.f}; // ~194 kts groundspeed
    hud.update(&e);
    bool foundLowIas = false, foundMach = false;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        if (el.text.find("IAS") != std::string_view::npos) {
            // Parse the kts value out of "IAS   NNNkts".
            int kts = std::atoi(el.text.data() + el.text.find("IAS") + 3);
            if (kts > 0 && kts < 194)
                foundLowIas = true;
        }
        if (el.text.find("M ") != std::string_view::npos && el.text.find("0.") != std::string_view::npos)
            foundMach = true;
    }
    CHECK(foundLowIas);
    CHECK(foundMach);
}

TEST_CASE("FlightHud includes altitude text") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 3500.0;
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("3500") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud includes heading text") {
    fl::FlightHud hud;
    auto e = makeEntry();
    // Identity quaternion = entity facing -Z = heading derived from yaw = 0
    e.orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("HDG") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud includes throttle text") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.throttle = 85;
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("85") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud includes Line element") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Line)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud text elements are HUD green") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e);
    // The first non-damage text element should be bright green (r~0, g~1, b~0)
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        if (el.text.find("DAMAGE") != std::string_view::npos)
            continue; // damage warning is red — skip
        CHECK(el.r < 0.1f);
        CHECK(el.g > 0.9f);
        CHECK(el.b < 0.1f);
        break;
    }
}

TEST_CASE("FlightHud damage warning is red") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.damageLevel = 1;
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.r > 0.9f && el.g < 0.5f)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud no damage element when intact") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.damageLevel = 0;
    hud.update(&e);
    bool redFound = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.r > 0.9f && el.g < 0.5f)
            redFound = true;
    CHECK_FALSE(redFound);
}

TEST_CASE("FlightHud time display shows 09:00 for mid-morning", "[flight_hud][weather]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e, 9.0f);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("09:00") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud time display shows 23:30 for late night", "[flight_hud][weather]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e, 23.5f);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("23:30") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud AGL text shows terrain-relative altitude") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 3500.0;
    hud.update(&e, 12.0f, 1000.0f);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("AGL") != std::string_view::npos &&
            el.text.find("2500") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud AGL equals MSL when terrain elevation is zero") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 3500.0;
    hud.update(&e, 12.0f, 0.0f);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("AGL") != std::string_view::npos &&
            el.text.find("3500") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud AGL element uses HUD green color") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 1000.0;
    hud.update(&e, 12.0f, 200.0f);
    bool found = false;
    for (const auto& el : hud.elements()) {
        if (el.type == HudElement::Type::Text && el.text.find("AGL") != std::string_view::npos) {
            CHECK(el.r < 0.1f);
            CHECK(el.g > 0.9f);
            CHECK(el.b < 0.1f);
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("FlightHud both ALT and AGL rows appear") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 5000.0;
    hud.update(&e, 12.0f, 1000.0f);
    bool altFound = false;
    bool aglFound = false;
    for (const auto& el : hud.elements()) {
        if (el.type == HudElement::Type::Text) {
            if (el.text.find("ALT") != std::string_view::npos)
                altFound = true;
            if (el.text.find("AGL") != std::string_view::npos)
                aglFound = true;
        }
    }
    CHECK(altFound);
    CHECK(aglFound);
}

TEST_CASE("FlightHud AGL is negative when below terrain level") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position.y = 100.0;
    hud.update(&e, 12.0f, 500.0f);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("AGL") != std::string_view::npos &&
            el.text.find('-') != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud alignment anchors: HDG and damage centered, clock right-aligned") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.damageLevel = 1;
    hud.update(&e, 9.0f);
    bool hdgFound = false;
    bool dmgFound = false;
    bool clockFound = false;
    bool iasFound = false;
    for (const auto& el : hud.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        if (el.text.find("HDG") != std::string_view::npos) {
            hdgFound = true;
            CHECK(el.align == HudAlign::Center);
            CHECK(el.x == Catch::Approx(0.5f));
        } else if (el.text.find("DAMAGE") != std::string_view::npos) {
            dmgFound = true;
            CHECK(el.align == HudAlign::Center);
            CHECK(el.x == Catch::Approx(0.5f));
        } else if (el.text.find("09:00") != std::string_view::npos) {
            clockFound = true;
            CHECK(el.align == HudAlign::Right);
            CHECK(el.x == Catch::Approx(0.98f));
        } else if (el.text.find("IAS") != std::string_view::npos) {
            iasFound = true;
            CHECK(el.align == HudAlign::Left);
        }
    }
    CHECK(hdgFound);
    CHECK(dmgFound);
    CHECK(clockFound);
    CHECK(iasFound);
}

TEST_CASE("FlightHud satisfies IHud via abstract pointer") {
    fl::FlightHud concrete;
    fl::IHud* hud = &concrete;
    hud->update(nullptr);
    CHECK(hud->elements().empty());
    auto entry = makeEntry();
    hud->update(&entry, 12.0f, 0.0f);
    CHECK_FALSE(hud->elements().empty());
}

// ---------------------------------------------------------------------------
// Latency indicator tests (#382)
// ---------------------------------------------------------------------------

TEST_CASE("FlightHud latency element shown when showLatency=true and latencyMs nonzero", "[flight_hud][latency]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e, 12.0f, 0.0f, 120u, true);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("120") != std::string_view::npos &&
            el.text.find("ms") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud latency element not shown when showLatency=false", "[flight_hud][latency]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e, 12.0f, 0.0f, 120u, false);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("ms") != std::string_view::npos)
            found = true;
    CHECK_FALSE(found);
}

TEST_CASE("FlightHud latency element not shown when latencyMs is zero", "[flight_hud][latency]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    hud.update(&e, 12.0f, 0.0f, 0u, true);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("ms") != std::string_view::npos)
            found = true;
    CHECK_FALSE(found);
}

// ---------------------------------------------------------------------------
// Radial artificial-horizon / pitch reference (#479)
// ---------------------------------------------------------------------------

// The artificial horizon is the Line element near screen centre (y in [0.2, 0.8]); the heading
// tape underline sits at y ~= 0.97.
static const HudElement* findHorizon(const fl::FlightHud& hud) {
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Line && el.y > 0.2f && el.y < 0.8f)
            return &el;
    return nullptr;
}

TEST_CASE("FlightHud shows a pitch readout", "[flight_hud][spherical]") {
    fl::FlightHud hud;
    auto e = makeEntry(); // identity orientation, near the world origin -> pitch ~ 0
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("PTCH") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("FlightHud level flight puts the horizon through screen centre", "[flight_hud][spherical]") {
    fl::FlightHud hud;
    auto e = makeEntry(); // identity orientation: wings level, nose on the horizon
    hud.update(&e);
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    const float midY = 0.5f * (h->y + h->y2);
    CHECK(midY == Catch::Approx(0.5f).margin(1e-3f));
    // Wings level -> the two endpoints share the same height.
    CHECK(h->y == Catch::Approx(h->y2).margin(1e-3f));
}

TEST_CASE("FlightHud nose-up drops the horizon below centre", "[flight_hud][spherical]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    // Rotate the nose up ~30 deg: rotation about body-right (+Z) tilts forward +X toward +Y (up).
    e.orientation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 0.f, 1.f));
    hud.update(&e);
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    const float midY = 0.5f * (h->y + h->y2);
    CHECK(midY > 0.5f); // screen y grows downward, so a nose-up horizon sits below centre
}

TEST_CASE("FlightHud bank tilts the horizon line", "[flight_hud][spherical]") {
    fl::FlightHud hud;
    auto e = makeEntry();
    // Roll ~30 deg about the forward (+X) axis.
    e.orientation = glm::angleAxis(glm::radians(30.f), glm::vec3(1.f, 0.f, 0.f));
    hud.update(&e);
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    CHECK(std::abs(h->y - h->y2) > 1e-3f); // banked -> endpoints at different heights
}

TEST_CASE("FlightHud attitude stays correct far from the world origin", "[flight_hud][spherical]") {
    // Equator point on Earth (Geodetic.h): world (0, -R, R). Local up = +Z there.
    constexpr double R = fl::kEarthRadiusM;
    fl::FlightHud hud;
    auto e = makeEntry();
    e.position = {0.0, -R, R};
    // Wings level on the LOCAL up (+Z): rotate body up +Y -> +Z via +90 deg about forward +X.
    e.orientation = glm::angleAxis(glm::radians(90.f), glm::vec3(1.f, 0.f, 0.f));
    hud.update(&e, 12.0f, 0.0f, 0u, false, R);
    const HudElement* h = findHorizon(hud);
    REQUIRE(h != nullptr);
    // Level far from origin -> horizon centred and untilted (would be wrong with a world-Y horizon).
    CHECK(0.5f * (h->y + h->y2) == Catch::Approx(0.5f).margin(2e-3f));
    CHECK(h->y == Catch::Approx(h->y2).margin(2e-3f));
}
