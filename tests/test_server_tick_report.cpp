// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/ServerTickReport.h"
#include "perf/TickProfiler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace fl;
using Catch::Approx;

namespace {
ServerTickReport sample() {
    ServerTickReport r;
    r.tickHz = 59.83;
    r.ticksSampled = 3600;
    r.ticksTotal = 123456;
    r.windowSeconds = 60.17;
    r.peers = 96;
    r.entities = 130;
    r.total = {1.0, 2.5, 9.0, 4.0, 6.5, 0.5};
    r.phases[static_cast<int>(TickPhase::Integrate)] = {0.1, 0.8, 2.0, 1.4, 1.8, 0.2};
    r.phases[static_cast<int>(TickPhase::Ai)] = {0.0, 0.3, 1.1, 0.6, 0.9, 0.1};
    r.phases[static_cast<int>(TickPhase::Serialize)] = {0.2, 1.0, 3.0, 2.0, 2.6, 0.4};
    r.other = {0.0, 0.4, 1.0, 0.7, 0.9, 0.1};
    return r;
}
} // namespace

TEST_CASE("ServerTickReport JSON round-trips", "[servertick]") {
    const ServerTickReport in = sample();
    const std::string json = toJson(in);

    ServerTickReport out;
    REQUIRE(fromJson(json, out));

    CHECK(out.schemaVersion == in.schemaVersion);
    CHECK(out.tickHz == Approx(in.tickHz).margin(1e-3));
    CHECK(out.ticksSampled == in.ticksSampled);
    CHECK(out.ticksTotal == in.ticksTotal);
    CHECK(out.windowSeconds == Approx(in.windowSeconds).margin(1e-3));
    CHECK(out.peers == in.peers);
    CHECK(out.entities == in.entities);
    CHECK(out.total.mean == Approx(in.total.mean).margin(1e-3));
    CHECK(out.total.p99 == Approx(in.total.p99).margin(1e-3));
    CHECK(out.phases[static_cast<int>(TickPhase::Integrate)].max ==
          Approx(in.phases[static_cast<int>(TickPhase::Integrate)].max).margin(1e-3));
    CHECK(out.phases[static_cast<int>(TickPhase::Serialize)].mean ==
          Approx(in.phases[static_cast<int>(TickPhase::Serialize)].mean).margin(1e-3));
    CHECK(out.other.mean == Approx(in.other.mean).margin(1e-3));
}

TEST_CASE("ServerTickReport toJson nesting indent is valid", "[servertick]") {
    const std::string nested = toJson(sample(), 2);
    // Indented form still parses back identically.
    ServerTickReport out;
    REQUIRE(fromJson(nested, out));
    CHECK(out.peers == 96);
}

TEST_CASE("makeServerTickReport maps a TickBudget plus counts", "[servertick]") {
    TickBudget b;
    b.tickHz = 60.0;
    b.ticksSampled = 100;
    b.ticksTotal = 200;
    b.windowSeconds = 1.65;
    b.total = {0.5, 1.0, 2.0, 1.5, 1.9, 0.1};
    b.phases[static_cast<int>(TickPhase::Ai)] = {0.0, 0.2, 0.5, 0.4, 0.45, 0.05};

    const ServerTickReport r = makeServerTickReport(b, 42, 7);
    CHECK(r.peers == 42);
    CHECK(r.entities == 7u);
    CHECK(r.tickHz == Approx(60.0));
    CHECK(r.ticksSampled == 100);
    CHECK(r.ticksTotal == 200);
    CHECK(r.total.p99 == Approx(1.9));
    CHECK(r.phases[static_cast<int>(TickPhase::Ai)].mean == Approx(0.2));
}

TEST_CASE("fromJson is tolerant of malformed and partial input", "[servertick]") {
    SECTION("empty string yields no fields") {
        ServerTickReport r;
        CHECK_FALSE(fromJson("", r));
    }
    SECTION("garbage yields no recognised fields") {
        ServerTickReport r;
        CHECK_FALSE(fromJson("not json at all {{{", r));
    }
    SECTION("partial object parses what it can") {
        ServerTickReport r;
        REQUIRE(fromJson("{ \"tick_hz\": 58.25, \"peers\": 12 }", r));
        CHECK(r.tickHz == Approx(58.25));
        CHECK(r.peers == 12);
        // Unspecified fields keep defaults.
        CHECK(r.entities == 0u);
        CHECK(r.total.mean == Approx(0.0));
    }
    SECTION("missing stat sub-keys keep stat defaults") {
        ServerTickReport r;
        REQUIRE(fromJson("{ \"tick_ms\": { \"mean\": 3.5 } }", r));
        CHECK(r.total.mean == Approx(3.5));
        CHECK(r.total.p99 == Approx(0.0));
    }
}

TEST_CASE("loadServerMetrics returns nullopt for a missing file", "[servertick]") {
    CHECK_FALSE(loadServerMetrics("/nonexistent/path/does/not/exist.json").has_value());
}
