// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated characterisation benchmark for the flight integrator (#1298), in the #573 pattern: [.]-hidden
// so the default ctest run skips it, run explicitly by name.
//
// It exists to answer one question with a number instead of a guess: what does `-ffp-contract=off`
// cost the sim's hot path on a platform that HAS fused multiply-add? On x86-64 the SSE2 baseline has
// no FMA, so the flag is a no-op there and this benchmark measures nothing interesting; on ARM64
// (Apple Silicon -- the macOS CI runner) contraction is on by default and the flag gives it up.
// .github/workflows/fp-contract-probe.yml builds this twice on that runner and prints both numbers.
//
// Measured 2026-09-15 (run 34988877661, Apple M1 virtual, Apple clang 21, three alternating passes):
// default 325.5 / 312.8 / 337.4 ns median (best 258), -ffp-contract=off 346.6 / 470.2 / 324.3 (best
// 286) -- about 6 % on the best-of runs, inside the VM's own noise. Fingerprints: ARM64 default
// ee35b5b6c9cf8e77, ARM64 off f6206c02a72d9bd4, x86-64 release ec747d35398204c8 -- THREE different
// results, so turning contraction off does not make ARM64 agree with x86-64 either (libm). Decision:
// the pin is NOT taken; it costs the hot path and buys no portability. Re-run the probe before
// revisiting that.
//
// The workload is the integrator alone -- no controllers, no world, no entity manager -- because that
// is where the transcendentals and the quaternion algebra #1298 measured live, and because anything
// wider would put the flag's cost behind allocation and cache noise it does not touch.
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace fl;

namespace {

constexpr int kAircraft = 256;
constexpr int kSteps = 3600; // 60 s at 60 Hz
constexpr int kRounds = 5;
constexpr float kDt = 1.f / 60.f;

FlightState levelState(int i) {
    FlightState s;
    s.pos_world[0] = 500.0 * i;
    s.pos_world[1] = 3000.0 + 10.0 * i;
    s.pos_world[2] = -500.0 * i;
    s.vel_body[0] = 150.f + static_cast<float>(i % 7) * 5.f;
    s.quat[3] = 1.f;
    s.throttle_actual = 0.6f;
    return s;
}

// A gentle, deterministic stick so every aircraft keeps manoeuvring for the whole run: a steady state
// would let the integrator settle into a path whose cost says nothing about a live match.
ControlInput stickAt(int aircraft, int step) {
    const float t = static_cast<float>(step) * kDt;
    const float phase = static_cast<float>(aircraft) * 0.37f;
    ControlInput c;
    c.throttle = 0.6f + 0.3f * std::sin(0.11f * t + phase);
    c.elevator = 0.25f * std::sin(0.23f * t + phase);
    c.aileron = 0.35f * std::sin(0.17f * t + phase * 1.3f);
    c.rudder = 0.05f * std::sin(0.31f * t);
    return c;
}

// FNV-1a over the final positions and velocities: keeps the work observable (nothing is dead code to
// the optimiser) and doubles as the fingerprint that shows two builds did NOT compute the same bits.
uint64_t fingerprint(const std::vector<std::unique_ptr<FlightIntegrator>>& fleet) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& f : fleet) {
        const FlightState& s = f->state();
        mix(s.pos_world, sizeof(s.pos_world));
        mix(s.vel_body, sizeof(s.vel_body));
        mix(s.quat, sizeof(s.quat));
    }
    return h;
}

} // namespace

TEST_CASE("FlightIntegrator scale: cost per aircraft-step, and the bit fingerprint of the run (#1298)",
          "[.][scale][flight]") {
    const auto model = BuiltinFlightModel::get();
    REQUIRE(model);

    std::vector<double> nsPerStep;
    uint64_t fp = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::unique_ptr<FlightIntegrator>> fleet;
        fleet.reserve(kAircraft);
        for (int i = 0; i < kAircraft; ++i) {
            fleet.push_back(std::make_unique<FlightIntegrator>(model));
            fleet.back()->reset(levelState(i));
        }
        const PayloadEffect payload{};

        const auto t0 = std::chrono::steady_clock::now();
        for (int step = 0; step < kSteps; ++step)
            for (int i = 0; i < kAircraft; ++i)
                fleet[i]->step(kDt, stickAt(i, step), payload);
        const auto t1 = std::chrono::steady_clock::now();

        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        nsPerStep.push_back(ns / (static_cast<double>(kAircraft) * kSteps));
        const uint64_t thisFp = fingerprint(fleet);
        if (round == 0)
            fp = thisFp;
        else
            CHECK(thisFp == fp); // deterministic within one build, or the number means nothing
    }

    std::vector<double> sorted = nsPerStep;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    std::printf("FL_FLIGHT_BENCH aircraft=%d steps=%d rounds=%d ns_per_aircraft_step_median=%.1f min=%.1f max=%.1f "
                "fingerprint=%016llx\n",
                kAircraft, kSteps, kRounds, median, sorted.front(), sorted.back(), static_cast<unsigned long long>(fp));
    CHECK(median > 0.0);
}
