// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "flight/Geodetic.h"
#include "render/AirportRenderer.h"
#include "render/RunwaySurfaceMap.h"
#include "render/SurfaceType.h"
#include "world/AirportRegistry.h"

#include <glm/geometric.hpp>
#include <vector>

using namespace fl;
using Catch::Approx;

namespace {
// Resolve one runway on the sphere via the registry so the geometry is realistic.
ResolvedRunway resolveOneRunway(RunwaySurface surface, float lengthM = 3000.f, float widthM = 60.f) {
    AirportDef def;
    def.id = "t";
    def.name = "t";
    def.useWorldXZ = true;
    def.worldX = 0.0;
    def.worldZ = 0.0;
    def.elevationM = 100.0;
    def.runways.push_back(RunwayDef{90.f, lengthM, widthM, surface});
    AirportRegistry reg;
    reg.load({def}, kEarthRadiusM, nullptr);
    return reg.byId("t")->runways.at(0);
}
} // namespace

TEST_CASE("surfaceTypeForRunway is total and correct", "[airport][render]") {
    CHECK(surfaceTypeForRunway(RunwaySurface::Concrete) == SurfaceType::Concrete);
    CHECK(surfaceTypeForRunway(RunwaySurface::Asphalt) == SurfaceType::Asphalt);
    CHECK(surfaceTypeForRunway(RunwaySurface::Grass) == SurfaceType::Grass);
    CHECK(surfaceTypeForRunway(RunwaySurface::Gravel) == SurfaceType::Gravel);
    CHECK(surfaceTypeForRunway(RunwaySurface::Water) == SurfaceType::Water);
    CHECK(surfaceTypeForRunway(RunwaySurface::Deck) == SurfaceType::Deck);
}

TEST_CASE("groundFrictionFor differentiates paved and unpaved", "[airport][physics]") {
    CHECK(groundFrictionFor(SurfaceType::Concrete).extraRollingPerSec == Approx(0.f));
    CHECK(groundFrictionFor(SurfaceType::Asphalt).extraRollingPerSec == Approx(0.f));
    CHECK(groundFrictionFor(SurfaceType::Deck).extraRollingPerSec == Approx(0.f));
    CHECK(groundFrictionFor(SurfaceType::Unknown).extraRollingPerSec == Approx(0.f));
    CHECK(groundFrictionFor(SurfaceType::Grass).extraRollingPerSec > 0.f);
    CHECK(groundFrictionFor(SurfaceType::Gravel).extraRollingPerSec > 0.f);
    // Grass drags harder than gravel; water (a ditching) hardest of all.
    CHECK(groundFrictionFor(SurfaceType::Grass).extraRollingPerSec >
          groundFrictionFor(SurfaceType::Gravel).extraRollingPerSec);
    CHECK(groundFrictionFor(SurfaceType::Water).extraRollingPerSec >
          groundFrictionFor(SurfaceType::Grass).extraRollingPerSec);
}

TEST_CASE("runwaySlabVertices sit on the spherical datum with correct extents", "[airport][render]") {
    const ResolvedRunway rw = resolveOneRunway(RunwaySurface::Concrete, /*length=*/3000.f, /*width=*/60.f);
    const std::vector<glm::dvec3> verts = runwaySlabVertices(rw, /*elevationM=*/100.0, kEarthRadiusM);
    REQUIRE(!verts.empty());
    CHECK(verts.size() % 2 == 0); // pairs (left, right) per cross-section

    // Every vertex sits at ~ R + elevation + a small lift above the datum.
    for (const glm::dvec3& v : verts) {
        const double alt = geodeticAltitude(v.x, v.y, v.z, kEarthRadiusM);
        CHECK(alt == Approx(100.06).margin(0.02)); // elevation 100 + ~6 cm lift
    }
    // Width: the first cross-section's two edges are ~60 m apart.
    CHECK(glm::length(verts[1] - verts[0]) == Approx(60.0).margin(0.5));
    // Length: the centre of the first vs last cross-section spans ~3000 m.
    const glm::dvec3 firstMid = 0.5 * (verts[0] + verts[1]);
    const glm::dvec3 lastMid = 0.5 * (verts[verts.size() - 2] + verts[verts.size() - 1]);
    CHECK(glm::length(lastMid - firstMid) == Approx(3000.0).margin(2.0));
}

TEST_CASE("a Water runway produces no slab", "[airport][render]") {
    const ResolvedRunway rw = resolveOneRunway(RunwaySurface::Water);
    CHECK(runwaySlabVertices(rw, 0.0, kEarthRadiusM).empty());
    CHECK(buildRunwaySlabGlb(rw, 0.0, kEarthRadiusM, glm::dvec3{0.0}).empty());
}

TEST_CASE("buildRunwaySlabGlb yields a valid GLB header", "[airport][render]") {
    const ResolvedRunway rw = resolveOneRunway(RunwaySurface::Asphalt);
    const glm::dvec3 origin = 0.5 * (rw.threshold + rw.oppositeEnd);
    const std::vector<uint8_t> glb = buildRunwaySlabGlb(rw, 100.0, kEarthRadiusM, origin);
    REQUIRE(glb.size() > 12u);
    // glTF binary magic "glTF" little-endian.
    CHECK(glb[0] == 'g');
    CHECK(glb[1] == 'l');
    CHECK(glb[2] == 'T');
    CHECK(glb[3] == 'F');
}
