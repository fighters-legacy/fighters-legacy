// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/RenderSnapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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

TEST_CASE("FlightHud includes airspeed text") {
    fl::FlightHud hud;
    auto e = makeEntry();
    e.velocity = {0.f, 0.f, 100.f}; // 100 m/s ~ 194 kts
    hud.update(&e);
    bool found = false;
    for (const auto& el : hud.elements())
        if (el.type == HudElement::Type::Text && el.text.find("194") != std::string_view::npos)
            found = true;
    CHECK(found);
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
