// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine/math/Angles.h and engine/math/Units.h (#1246).
//
// These constants replaced ~40 hand-written spellings scattered across engine/, game/ and tools/.
// The consolidation was allowed to land WITHOUT a determinism re-bless on exactly one claim: every
// spelling it replaced rounds to the identical value at its own type, so no number in the sim moved.
//
// That claim is what this file pins. A comparison to a tolerance would not do it — the point is
// bit-for-bit identity, so each case compares the raw bytes. If someone later "tidies" a constant
// here, or writes `kPi<double>` where a float site wants `kPi<float>`, these fail rather than
// quietly shifting a flight model, a magnetic declination or a campaign frontline.

#include "math/Angles.h"
#include "math/Units.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <numbers>

namespace {

// Identical bytes, not "close enough". `==` would also do here, but memcmp says the intent out loud
// and does not invite anyone to relax it into an epsilon.
template <typename T> [[nodiscard]] bool sameBits(T a, T b) noexcept {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

} // namespace

TEST_CASE("the float deg->rad spellings this replaced are bit-identical", "[math_constants]") {
    // engine/flight/AeroForces.cpp, VesselForceModel.cpp, FixedWingForceModel.cpp
    REQUIRE(sameBits(fl::kDegToRad<float>, static_cast<float>(std::numbers::pi) / 180.f));
    // engine/flight/Trim.cpp
    REQUIRE(sameBits(fl::kDegToRad<float>, std::numbers::pi_v<float> / 180.f));
    // engine/flight/FlightIntegrator.cpp — the truncated literal
    REQUIRE(sameBits(fl::kDegToRad<float>, 0.0174532925f));
    // game/fighters-legacy/HeadTracker.h
    REQUIRE(sameBits(fl::kDegToRad<float>, 3.14159265f / 180.0f));
}

TEST_CASE("the double deg->rad spellings this replaced are bit-identical", "[math_constants]") {
    // engine/world/AirportCsvImport.cpp, AirportDefParser.cpp, SandboxHome.h, nav/MagneticModel.cpp
    REQUIRE(sameBits(fl::kDegToRad<double>, std::numbers::pi / 180.0));
    // engine/campaign/TheaterManifest.cpp, engine/mission/MissionParser.cpp
    REQUIRE(sameBits(fl::kDegToRad<double>, 3.14159265358979323846 / 180.0));
    // engine/weather/SolarPosition.h
    REQUIRE(sameBits(fl::kDegToRad<double>, 0.017453292519943295));

    REQUIRE(sameBits(fl::kRadToDeg<double>, 180.0 / std::numbers::pi_v<double>)); // script/LuaController.cpp
    REQUIRE(sameBits(fl::kRadToDeg<double>, 180.0 / 3.14159265358979323846));     // campaign/CampaignEngine.cpp
    REQUIRE(sameBits(fl::kRadToDeg<float>, 180.f / std::numbers::pi_v<float>));   // sensor/Detection.cpp
}

TEST_CASE("pi and two-pi match the literals the wrap sites used", "[math_constants]") {
    REQUIRE(sameBits(fl::kPi<float>, 3.14159265358979323846f));            // weapon/Turret.cpp
    REQUIRE(sameBits(fl::kPi<float>, 3.14159265f));                        // render/FlightHudMfd.cpp
    REQUIRE(sameBits(fl::kTwoPi<float>, 6.2831853f));                      // render/FlightHudMfd.cpp
    REQUIRE(sameBits(fl::kTwoPi<float>, 2.f * std::numbers::pi_v<float>)); // ai/Guidance.h
    REQUIRE(sameBits(fl::kTwoPi<double>, 2.0 * std::numbers::pi));         // campaign/Frontline.cpp

    // float pi rounds UP past double pi. This is why the type is spelled at every call site: a
    // float compared against the double constant answers differently at the boundary.
    REQUIRE(static_cast<double>(fl::kPi<float>) > fl::kPi<double>);
}

TEST_CASE("standard gravity is the same number at both widths", "[math_constants]") {
    REQUIRE(sameBits(fl::kG0<float>, 9.80665f));
    REQUIRE(sameBits(fl::kG0<double>, 9.80665));
}

TEST_CASE("the unit primaries are bit-identical, so content parses to the same SI value", "[math_constants]") {
    REQUIRE(sameBits(fl::kMetresPerNauticalMile<float>, 1852.f)); // weapon/sensor def parsers
    REQUIRE(sameBits(fl::kMetresPerFoot<float>, 0.3048f));
    REQUIRE(sameBits(fl::kMpsPerKnot<float>, 0.514444f)); // also flight/Trim.cpp's kKnotToMps
    REQUIRE(sameBits(fl::kKgPerPound<float>, 0.45359237f));

    // world/AirportCsvImport.cpp reads elevations as DOUBLE feet. Handing it the float constant
    // would have moved every imported airport elevation — the reason these are templated at all.
    REQUIRE(sameBits(fl::kMetresPerFoot<double>, 0.3048));
    REQUIRE_FALSE(sameBits(static_cast<double>(fl::kMetresPerFoot<float>), fl::kMetresPerFoot<double>));
}

TEST_CASE("the display inverses round-trip, which the old literals did not", "[math_constants]") {
    // The deliberate behaviour change in #1246. `1.94384f` and `2.20462f` were independent
    // approximations, not reciprocals: a speed authored in knots came back as a different number of
    // knots on the HUD. Deriving costs the sixth significant figure and buys back the round trip.
    REQUIRE_FALSE(sameBits(fl::kKnotsPerMps<float>, 1.94384f));
    REQUIRE_FALSE(sameBits(fl::kPoundsPerKg<float>, 2.20462f));

    // What "round-trip" means, at a speed and a weight a fighter actually flies at.
    REQUIRE(600.f * fl::kMpsPerKnot<float> * fl::kKnotsPerMps<float> == 600.f);
    REQUIRE(2000.f * fl::kKgPerPound<float> * fl::kPoundsPerKg<float> == 2000.f);

    // These two happen to land on the same float as the literals they replace, so the altitude and
    // range readouts do not move at all.
    REQUIRE(sameBits(fl::kFeetPerMetre<float>, 3.28084f));
    REQUIRE(sameBits(fl::kNauticalMilesPerMetre<float>, 1.f / 1852.0f));
}

TEST_CASE("wrapPi is closed at both ends and wrapTwoPi is half-open", "[math_constants]") {
    REQUIRE(fl::wrapPi(0.f) == 0.f);
    REQUIRE(fl::wrapPi(fl::kPi<float>) == fl::kPi<float>);   // +pi is left alone
    REQUIRE(fl::wrapPi(-fl::kPi<float>) == -fl::kPi<float>); // and so is -pi
    REQUIRE(fl::wrapPi(fl::kPi<float> + 1.f) < 0.f);
    REQUIRE(fl::wrapPi(fl::kTwoPi<float>) < 1e-6f);

    REQUIRE(fl::wrapTwoPi(0.0) == 0.0);
    REQUIRE(fl::wrapTwoPi(-0.5) > 5.7);                // east of the antimeridian, not negative
    REQUIRE(fl::wrapTwoPi(fl::kTwoPi<double>) == 0.0); // half-open: 2pi wraps to 0
    REQUIRE(fl::wrapTwoPi(fl::kTwoPi<double> + 1.0) == 1.0);
}
