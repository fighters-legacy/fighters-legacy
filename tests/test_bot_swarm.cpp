// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure-logic unit tests for the bot_swarm load harness: flight patterns + registry, the shared
// NetStats percentile math, CLI parsing, and metric aggregation + JSON. No sockets.
#include "IFlightPattern.h"
#include "NetStats.h"
#include "SwarmConfig.h"
#include "SwarmMetrics.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Flight patterns + registry
// ---------------------------------------------------------------------------

TEST_CASE("makePattern returns the built-ins and nullptr for unknown names", "[bot_swarm][pattern]") {
    for (const auto& name : patternNames()) {
        CHECK(isKnownPattern(name));
        CHECK(makePattern(name, 1u) != nullptr);
    }
    CHECK_FALSE(isKnownPattern("nope"));
    CHECK(makePattern("nope", 1u) == nullptr);
}

TEST_CASE("weave pattern is deterministic and spreads phase across clients", "[bot_swarm][pattern]") {
    WeavePattern a;
    WeavePattern b;
    // Same (t, index) -> identical output.
    CHECK(a.sample(1.5, 7).aileron == Catch::Approx(b.sample(1.5, 7).aileron));
    // Different client index -> different phase, so different aileron at the same time.
    CHECK(a.sample(1.5, 0).aileron != Catch::Approx(a.sample(1.5, 1).aileron));
    CHECK(a.sample(2.0, 3).throttle == Catch::Approx(0.7f));
}

TEST_CASE("idle pattern emits no input; level holds throttle", "[bot_swarm][pattern]") {
    IdlePattern idle;
    const BotControl ic = idle.sample(5.0, 2);
    CHECK(ic.throttle == Catch::Approx(0.0f));
    CHECK(ic.aileron == Catch::Approx(0.0f));
    CHECK(ic.buttons == 0);

    LevelPattern level;
    const BotControl lc = level.sample(5.0, 2);
    CHECK(lc.throttle == Catch::Approx(0.6f));
    CHECK(lc.aileron == Catch::Approx(0.0f));
}

TEST_CASE("aggressive pattern lights the afterburner and stays in range", "[bot_swarm][pattern]") {
    AggressivePattern p;
    const BotControl c = p.sample(3.0, 4);
    CHECK((c.buttons & 0x02) != 0); // afterburner bit
    CHECK(c.aileron <= 1.0f);
    CHECK(c.aileron >= -1.0f);
    CHECK(c.throttle == Catch::Approx(1.0f));
}

TEST_CASE("random pattern is reproducible for a seed and stays in range", "[bot_swarm][pattern]") {
    RandomPattern a(42u);
    RandomPattern b(42u);
    for (int i = 0; i < 50; ++i) {
        const BotControl ca = a.sample(static_cast<double>(i) * 0.1, 0);
        const BotControl cb = b.sample(static_cast<double>(i) * 0.1, 0);
        CHECK(ca.aileron == Catch::Approx(cb.aileron));
        CHECK(ca.throttle == Catch::Approx(cb.throttle));
        CHECK(ca.throttle >= 0.0f);
        CHECK(ca.throttle <= 1.0f);
        CHECK(ca.aileron >= -1.0f);
        CHECK(ca.aileron <= 1.0f);
    }
}

// ---------------------------------------------------------------------------
// NetStats
// ---------------------------------------------------------------------------

TEST_CASE("computeStats produces correct summary statistics", "[bot_swarm][stats]") {
    std::vector<double> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const Stats s = computeStats(v);
    CHECK(s.min == Catch::Approx(1.0));
    CHECK(s.max == Catch::Approx(10.0));
    CHECK(s.mean == Catch::Approx(5.5));
    CHECK(s.p95 >= 9.0); // nearest-rank, high tail
    CHECK(s.p99 >= 9.0);
}

TEST_CASE("computeStats on an empty set returns zeros", "[bot_swarm][stats]") {
    std::vector<double> v;
    const Stats s = computeStats(v);
    CHECK(s.min == Catch::Approx(0.0));
    CHECK(s.mean == Catch::Approx(0.0));
    CHECK(s.max == Catch::Approx(0.0));
}

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

static SwarmParseResult parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("bot_swarm"));
    for (auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    return parseSwarmArgs(static_cast<int>(argv.size()), argv.data());
}

TEST_CASE("parseSwarmArgs defaults are sensible", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.host == "127.0.0.1");
    CHECK(r.cfg.port == 4778);
    CHECK(r.cfg.clients == 32);
    CHECK(r.cfg.pattern == "weave");
    CHECK_FALSE(r.hostSet);
    CHECK_FALSE(r.portSet);
}

TEST_CASE("parseSwarmArgs reads positional host and port and flags", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"10.0.0.5", "5000", "--clients", "128", "--pattern", "aggressive", "--duration",
                                      "60", "--json", "out.json", "--threads", "4"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.host == "10.0.0.5");
    CHECK(r.cfg.port == 5000);
    CHECK(r.hostSet);
    CHECK(r.portSet);
    CHECK(r.cfg.clients == 128);
    CHECK(r.cfg.pattern == "aggressive");
    CHECK(r.cfg.durationS == 60);
    CHECK(r.cfg.jsonPath == "out.json");
    CHECK(r.cfg.threads == 4);
}

TEST_CASE("parseSwarmArgs rejects bad input", "[bot_swarm][config]") {
    CHECK(parse({"--clients", "0"}).status == ParseStatus::Error);
    CHECK(parse({"--pattern", "bogus"}).status == ParseStatus::Error);
    CHECK(parse({"--rate", "0"}).status == ParseStatus::Error);
    CHECK(parse({"--clients"}).status == ParseStatus::Error);    // missing value
    CHECK(parse({"--bogus-flag"}).status == ParseStatus::Error); // unknown flag
    CHECK(parse({"--help"}).status == ParseStatus::Help);
    CHECK(parse({"--version"}).status == ParseStatus::Version);
}

TEST_CASE("parseSwarmArgs parses assert thresholds with strtod", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--assert-min-tick-hz", "58.5", "--assert-max-kbs", "150"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMinTickHz == Catch::Approx(58.5));
    CHECK(r.cfg.assertMaxKbs == Catch::Approx(150.0));
}

// ---------------------------------------------------------------------------
// Metric aggregation + JSON
// ---------------------------------------------------------------------------

static ClientMetrics makeClient(uint64_t bytes, uint64_t firstTick, uint64_t lastTick, double firstWall,
                                double lastWall) {
    ClientMetrics m;
    m.connected = true;
    m.connectMs = 5.0;
    m.snapshotBytes = bytes;
    m.snapshotCount = lastTick - firstTick;
    m.firstSnapshotTick = firstTick;
    m.lastSnapshotTick = lastTick;
    m.firstSnapshotWall = firstWall;
    m.lastSnapshotWall = lastWall;
    m.rttMs = 12;
    m.rttValid = true;
    return m;
}

TEST_CASE("buildReport aggregates connected clients and computes tick-Hz + bandwidth", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 2;
    cfg.durationS = 10;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(45000, 0, 600, 0.0, 10.0)); // 600 ticks / 10 s = 60 Hz
    clients.push_back(makeClient(46000, 5, 605, 0.0, 10.0));

    const SwarmReport r = buildReport(cfg, clients, 10.0, {16.6, 16.7}, 1);
    CHECK(r.clientsRequested == 2);
    CHECK(r.clientsConnected == 2);
    CHECK(r.clientsRefused == 0);
    CHECK(r.tickHz.mean == Catch::Approx(60.0));
    CHECK(r.downstreamKbs.mean == Catch::Approx((45000.0 + 46000.0) / 2.0 / 10.0 / 1024.0).epsilon(0.01));
    CHECK(r.aggregateDownstreamMbs > 0.0);
}

TEST_CASE("buildReport counts refused and disconnected clients", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 3;
    cfg.durationS = 5;
    std::vector<ClientMetrics> clients(3);
    clients[0] = makeClient(1000, 0, 300, 0.0, 5.0);
    clients[1].connected = false;            // never connected -> refused
    clients[2].disconnectedDuringRun = true; // dropped mid-run
    clients[2].connected = false;

    const SwarmReport r = buildReport(cfg, clients, 5.0, {}, 1);
    CHECK(r.clientsConnected == 1);
    CHECK(r.clientsDisconnected == 1);
    CHECK(r.clientsRefused == 1);
}

TEST_CASE("buildReport applies assert thresholds", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.durationS = 10;
    cfg.assertMinTickHz = 50.0;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0)); // 60 Hz

    CHECK(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed); // 60 >= 50
    cfg.assertMinTickHz = 70.0;
    CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed); // 60 < 70
}

TEST_CASE("reportToJson emits the versioned schema and key fields", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.host = "127.0.0.1";
    cfg.port = 4778;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));
    const std::string json = reportToJson(buildReport(cfg, clients, 10.0, {16.6}, 1));

    CHECK(json.find("\"schema_version\": 1") != std::string::npos);
    CHECK(json.find("\"observed_server_tick_hz\"") != std::string::npos);
    CHECK(json.find("\"downstream_kbs_per_client\"") != std::string::npos);
    CHECK(json.find("\"clients_connected\": 1") != std::string::npos);
    CHECK(json.find("\"asserts\"") != std::string::npos);
}
