// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Load-test affordance (#573/#580): deterministic placement + controller-mix assignment +
// projectile-churn pacing for the server-side entities that stress the entity pool, SpatialIndex,
// and AI phase at scale. Pure logic, extracted from fl-server main() so it is unit-testable
// (mirrors resolveWorkerCount / clampCatchupTicks). A TESTING AFFORDANCE, NOT A CAPACITY GUARANTEE.

namespace fl {

// Places a planar (x, z) column at `aglM` above the terrain AT THAT COLUMN, returning the world
// position. The caller owns the terrain query, so this header stays free of the renderer: fl-server
// passes a lambda over TerrainStreamer::heightAt mapped onto the near side of the sphere.
using TestSpawnSurfaceFn = std::function<std::array<double, 3>(double x, double z, double aglM)>;

// A flat world at a fixed datum elevation. This is the PRE-#1137 behaviour and exists for tests and
// for callers with no terrain at all — never for the load spawn, whose whole problem was that the
// origin's ground elevation says nothing about the ground 25 km away.
inline TestSpawnSurfaceFn flatTestSpawnSurface(double baseElevM) {
    return [baseElevM](double x, double z, double aglM) -> std::array<double, 3> { return {x, baseElevM + aglM, z}; };
}

// Returns `count` world positions {x, y, z} spread over a disk of radius `spreadM` (metres) around
// the origin via a phyllotaxis (sunflower) pattern — an even, deterministic fill that populates many
// SpatialIndex cells rather than clustering into one. Each entity is placed at `aglM` above ITS OWN
// local terrain via `surfaceFn`. Deterministic: identical input (and a deterministic surfaceFn)
// yields identical output on every platform.
//
// ⚠ The altitude used to be measured above the ORIGIN's ground for every entity (#1137). Over a wide
// spread anything above higher ground started INSIDE a hill and died on contact — silently, as a
// slowly shrinking population rather than an error. A 64-entity spread over 50 km at 500 m drained
// to 49 within 90 s, which made the scale-gate baseline a function of run duration and luck.
inline std::vector<std::array<double, 3>> testSpawnPositions(uint32_t count, double spreadM, double aglM,
                                                             const TestSpawnSurfaceFn& surfaceFn) {
    std::vector<std::array<double, 3>> out;
    out.reserve(count);
    // Golden angle in radians (pi * (3 - sqrt(5))).
    constexpr double kGoldenAngle = 2.399963229728653;
    const double denom = count > 0u ? static_cast<double>(count) : 1.0;
    for (uint32_t i = 0; i < count; ++i) {
        // sqrt distributes points uniformly by area; (i + 0.5)/count keeps the first point off the
        // exact centre (radius 0 would collapse onto the origin / the peer spawn).
        const double r = spreadM * std::sqrt((static_cast<double>(i) + 0.5) / denom);
        const double theta = static_cast<double>(i) * kGoldenAngle;
        out.push_back(surfaceFn(r * std::cos(theta), r * std::sin(theta), aglM));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Controller mix for the load-spawn (#580)
// ---------------------------------------------------------------------------

// The behaviors [world] test_spawn_ai_mix may name. Each stresses a different AI-phase cost:
//   loiter — pure guidance math, no lookups (the #573 baseline; isolates pool+index cost).
//   pursuit — EntityManager::get() on a moving target every tick.
//   patrol — patrol_attack StateMachineController: SpatialIndex::queryRadius() condition checks
//            every tick (the expensive AI path, and what the governor's AI stride decimates).
inline bool isKnownTestSpawnBehavior(std::string_view name) {
    return name == "loiter" || name == "pursuit" || name == "patrol";
}

struct TestSpawnMixEntry {
    std::string behavior;
    int weight{0};
};

// Parses "loiter:70,pursuit:20,patrol:10" into weighted entries (the #560 --pattern-mix grammar).
// Returns false with `err` set on a malformed entry, unknown behavior, or non-positive weight.
// An empty spec is invalid here — callers treat the empty *config default* as all-loiter upstream.
inline bool parseTestSpawnMix(std::string_view spec, std::vector<TestSpawnMixEntry>& out, std::string& err) {
    out.clear();
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::string_view tok =
            spec.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (tok.empty()) {
            err = "empty entry in test_spawn_ai_mix";
            return false;
        }
        const std::size_t colon = tok.find(':');
        if (colon == std::string_view::npos) {
            err = "test_spawn_ai_mix entry needs behavior:weight";
            return false;
        }
        std::string name(tok.substr(0, colon));
        const std::string wstr(tok.substr(colon + 1));
        if (!isKnownTestSpawnBehavior(name)) {
            err = "unknown behavior in test_spawn_ai_mix: " + name + " (loiter|pursuit|patrol)";
            return false;
        }
        char* end = nullptr;
        const long w = std::strtol(wstr.c_str(), &end, 10);
        if (end == wstr.c_str() || *end != '\0' || w <= 0) {
            err = "test_spawn_ai_mix weight must be a positive integer: " + wstr;
            return false;
        }
        out.push_back({std::move(name), static_cast<int>(w)});
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    if (out.empty()) {
        err = "empty test_spawn_ai_mix";
        return false;
    }
    return true;
}

// Deterministically assigns entity `index` of `total` to a mix entry by its cumulative-weight
// bucket (floor(i * totalWeight / total)) — counts match the weight fractions within rounding,
// no RNG, reproducible across runs/platforms (the #560 assignMixPattern scheme).
inline const std::string& assignTestSpawnBehavior(const std::vector<TestSpawnMixEntry>& mix, uint32_t index,
                                                  uint32_t total) {
    long long totalWeight = 0;
    for (const auto& e : mix)
        totalWeight += e.weight;
    const long long key =
        (total > 0u) ? (static_cast<long long>(index) * totalWeight / static_cast<long long>(total)) : 0;
    long long cum = 0;
    for (const auto& e : mix) {
        cum += e.weight;
        if (key < cum)
            return e.behavior;
    }
    return mix.back().behavior;
}

// ---------------------------------------------------------------------------
// Projectile churn (#580)
// ---------------------------------------------------------------------------

// Fractional-accumulator spawn pacing (the particle-spawner idiom): returns how many projectiles
// to spawn this tick for `ratePerSecond`, carrying the remainder in `accum` so sub-tick rates
// (< 60/s) still emit correctly over time. Pure; deterministic for a given call sequence.
inline uint32_t churnSpawnCount(double& accum, double ratePerSecond, double dtSeconds) {
    if (ratePerSecond <= 0.0 || dtSeconds <= 0.0)
        return 0u;
    accum += ratePerSecond * dtSeconds;
    const double whole = std::floor(accum);
    accum -= whole;
    return static_cast<uint32_t>(whole);
}

// Position for the k-th projectile ever spawned: the same phyllotaxis fill as testSpawnPositions,
// walked by a monotonically increasing counter modulo a fixed ring so churned entities keep landing
// in *different* SpatialIndex cells (fresh cell traffic, not one hot cell). Deterministic.
// Terrain-relative per position for the same reason as testSpawnPositions (#1137) — it walks the
// same 50 km spread off the same test_spawn_agl_m knob, so it had the same trap.
inline std::array<double, 3> testProjectilePosition(uint64_t counter, double spreadM, double aglM,
                                                    const TestSpawnSurfaceFn& surfaceFn) {
    constexpr double kGoldenAngle = 2.399963229728653;
    constexpr uint64_t kRing = 4096; // radius pattern repeats; angle keeps walking the golden spiral
    const double frac = (static_cast<double>(counter % kRing) + 0.5) / static_cast<double>(kRing);
    const double r = spreadM * std::sqrt(frac);
    const double theta = static_cast<double>(counter) * kGoldenAngle;
    return surfaceFn(r * std::cos(theta), r * std::sin(theta), aglM);
}

} // namespace fl
