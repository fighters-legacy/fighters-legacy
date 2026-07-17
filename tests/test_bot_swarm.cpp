// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure-logic unit tests for the bot_swarm load harness: flight patterns + registry, the shared
// NetStats percentile math, CLI parsing, metric aggregation + JSON, and the lossy-link
// drop/delay policy (#714). No sockets (LossyProxy's relay is exercised by the gate itself).
#include "IFlightPattern.h"
#include "LossyLink.h"
#include "NetStats.h"
#include "SwarmConfig.h"
#include "SwarmMetrics.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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
// Trace replay + weighted pattern mix (#560)
// ---------------------------------------------------------------------------

TEST_CASE("isTracePattern recognises trace:<file> specs", "[bot_swarm][pattern][trace]") {
    CHECK(isTracePattern("trace:/tmp/a.flit"));
    CHECK(tracePatternPath("trace:foo.flit") == "foo.flit");
    CHECK_FALSE(isTracePattern("trace:")); // empty path
    CHECK_FALSE(isTracePattern("weave"));
    CHECK_FALSE(isKnownPattern("trace:foo.flit")); // trace is not a built-in registry name
}

TEST_CASE("TracePattern replays an in-memory trace, phases per client, and loops", "[bot_swarm][pattern][trace]") {
    auto trace = std::make_shared<InputTrace>();
    trace->tickRate = 60u;
    for (uint32_t i = 0; i < 4; ++i)
        trace->records.push_back(InputTraceRecord{i, 0.1f * static_cast<float>(i), 0.f, 0.f, 0.f, i});
    TracePattern p(trace);

    // t=0, client 0 -> record 0.
    CHECK(p.sample(0.0, 0).throttle == Catch::Approx(0.0f));
    CHECK(p.sample(0.0, 0).buttons == 0u);
    // Each client offsets its cursor by its index: client 1 at t=0 -> record 1.
    CHECK(p.sample(0.0, 1).throttle == Catch::Approx(0.1f));
    CHECK(p.sample(0.0, 1).buttons == 1u);
    // Wall time advances the cursor at tickRate: client 0 at t = 2/60 s -> record 2.
    CHECK(p.sample(2.0 / 60.0, 0).throttle == Catch::Approx(0.2f));
    // Playback loops: 4 records, so index 5 wraps to record 1.
    CHECK(p.sample(0.0, 5).buttons == 1u);
}

TEST_CASE("TracePattern on an empty trace yields neutral input", "[bot_swarm][pattern][trace]") {
    auto trace = std::make_shared<InputTrace>();
    trace->tickRate = 60u;
    TracePattern p(trace);
    const BotControl c = p.sample(1.0, 3);
    CHECK(c.throttle == Catch::Approx(0.0f));
    CHECK(c.buttons == 0u);
}

TEST_CASE("parsePatternMix parses valid specs and rejects malformed ones", "[bot_swarm][pattern][mix]") {
    std::vector<PatternMixEntry> mix;
    std::string err;

    REQUIRE(parsePatternMix("weave:80,aggressive:15,idle:5", mix, err));
    REQUIRE(mix.size() == 3);
    CHECK(mix[0].name == "weave");
    CHECK(mix[0].weight == 80);
    CHECK(mix[2].name == "idle");
    CHECK(mix[2].weight == 5);

    CHECK(parsePatternMix("weave:100", mix, err));

    CHECK_FALSE(parsePatternMix("weave", mix, err));     // missing :weight
    CHECK_FALSE(parsePatternMix("nope:50", mix, err));   // unknown pattern
    CHECK_FALSE(parsePatternMix("weave:0", mix, err));   // non-positive weight
    CHECK_FALSE(parsePatternMix("weave:-5", mix, err));  // negative weight
    CHECK_FALSE(parsePatternMix("weave:80,", mix, err)); // trailing empty entry
    CHECK_FALSE(parsePatternMix("weave:1.5", mix, err)); // non-integer weight
    CHECK_FALSE(parsePatternMix("", mix, err));          // empty spec
}

TEST_CASE("assignMixPattern distributes clients deterministically by weight", "[bot_swarm][pattern][mix]") {
    std::vector<PatternMixEntry> mix;
    std::string err;
    REQUIRE(parsePatternMix("weave:80,aggressive:20", mix, err));

    const int n = 100;
    int weave = 0, aggressive = 0;
    for (int i = 0; i < n; ++i) {
        const std::string& name = assignMixPattern(mix, static_cast<uint32_t>(i), n);
        if (name == "weave")
            ++weave;
        else if (name == "aggressive")
            ++aggressive;
    }
    CHECK(weave == 80);
    CHECK(aggressive == 20);

    // Deterministic: same index -> same assignment on a repeat pass.
    CHECK(assignMixPattern(mix, 42u, n) == assignMixPattern(mix, 42u, n));
}

TEST_CASE("SwarmPatternPlan selects the active mode and builds patterns", "[bot_swarm][pattern][mix]") {
    // Single built-in.
    SwarmPatternPlan single;
    single.single = "weave";
    single.totalClients = 4;
    CHECK(single.make(0) != nullptr);

    // Weighted mix -> non-null instances across the swarm.
    SwarmPatternPlan mixPlan;
    mixPlan.totalClients = 10;
    std::string err;
    REQUIRE(parsePatternMix("weave:50,idle:50", mixPlan.mix, err));
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(mixPlan.make(i) != nullptr);

    // Trace -> TracePattern instances.
    auto trace = std::make_shared<InputTrace>();
    trace->tickRate = 60u;
    trace->records.push_back(InputTraceRecord{0, 0.6f, 0.f, 0.f, 0.f, 0u});
    SwarmPatternPlan tracePlan;
    tracePlan.trace = trace;
    tracePlan.totalClients = 2;
    auto pat = tracePlan.make(0);
    REQUIRE(pat != nullptr);
    CHECK(pat->sample(0.0, 0).throttle == Catch::Approx(0.6f));
}

// ---------------------------------------------------------------------------
// Lossy-link drop/delay policy (#714)
// ---------------------------------------------------------------------------

TEST_CASE("LossyLink window boundaries and disabled schedules", "[bot_swarm][lossy]") {
    LossySchedule s;
    s.degradeStartS = 10.0;
    s.degradeDurationS = 5.0;
    s.lossFraction = 0.f;
    s.delayMs = 100;
    LossyLink link(s, 42u);

    CHECK_FALSE(link.degradedAt(9.99));
    CHECK(link.degradedAt(10.0)); // window start is inclusive
    CHECK(link.degradedAt(14.99));
    CHECK_FALSE(link.degradedAt(15.0)); // window end is exclusive

    // Outside the window every datagram forwards immediately (delay 0, never dropped).
    auto before = link.classify(5.0);
    REQUIRE(before.has_value());
    CHECK(*before == 0u);
    // Inside the window (no loss configured) every datagram carries the added delay.
    auto during = link.classify(12.0);
    REQUIRE(during.has_value());
    CHECK(*during == 100u);

    // A zero-duration schedule is a no-op policy.
    LossySchedule off;
    off.lossFraction = 0.5f;
    off.delayMs = 500; // irrelevant: durationS 0 disables
    CHECK_FALSE(off.enabled());
    LossyLink offLink(off, 1u);
    CHECK_FALSE(offLink.degradedAt(100.0));
    auto v = offLink.classify(100.0);
    REQUIRE(v.has_value());
    CHECK(*v == 0u);
}

TEST_CASE("LossyLink drops roughly lossFraction of datagrams, deterministically per seed", "[bot_swarm][lossy]") {
    LossySchedule s;
    s.degradeStartS = 0.0;
    s.degradeDurationS = 1000.0;
    s.lossFraction = 0.30f;
    s.delayMs = 0;

    auto countDrops = [&](uint32_t seed) {
        LossyLink link(s, seed);
        int drops = 0;
        for (int i = 0; i < 10000; ++i)
            if (!link.classify(1.0))
                ++drops;
        return drops;
    };

    const int dropsA = countDrops(7u);
    // ~30% of 10k, generous tolerance — this is a sanity band, not a distribution test.
    CHECK(dropsA > 2600);
    CHECK(dropsA < 3400);
    // Same seed -> identical decision sequence (reproducible runs).
    CHECK(dropsA == countDrops(7u));
}

TEST_CASE("LossyLink RNG only advances inside the degraded window", "[bot_swarm][lossy]") {
    LossySchedule s;
    s.degradeStartS = 10.0;
    s.degradeDurationS = 1.0;
    s.lossFraction = 0.5f;
    LossyLink a(s, 3u);
    LossyLink b(s, 3u);

    // Feed `a` a burst of clean-phase datagrams first; decisions inside the window must still
    // match `b` exactly (clean traffic must not perturb the degraded-phase sequence).
    for (int i = 0; i < 100; ++i)
        (void)a.classify(1.0);
    for (int i = 0; i < 50; ++i)
        CHECK(a.classify(10.5).has_value() == b.classify(10.5).has_value());
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

TEST_CASE("parseSwarmArgs accepts trace patterns and pattern mixes", "[bot_swarm][config][trace][mix]") {
    // trace:<file> is a valid --pattern (the file is loaded at run time, not parse time).
    const SwarmParseResult t = parse({"--pattern", "trace:session.flit"});
    REQUIRE(t.status == ParseStatus::Ok);
    CHECK(t.cfg.pattern == "trace:session.flit");

    const SwarmParseResult m = parse({"--pattern-mix", "weave:80,aggressive:20"});
    REQUIRE(m.status == ParseStatus::Ok);
    CHECK(m.cfg.patternMix == "weave:80,aggressive:20");

    // A malformed mix is rejected at parse time.
    CHECK(parse({"--pattern-mix", "weave:80,nope:20"}).status == ParseStatus::Error);
    CHECK(parse({"--pattern-mix", "garbage"}).status == ParseStatus::Error);
    CHECK(parse({"--pattern-mix"}).status == ParseStatus::Error); // missing value
}

TEST_CASE("parseSwarmArgs parses assert thresholds with strtod", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--assert-min-tick-hz", "58.5", "--assert-max-kbs", "150"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMinTickHz == Catch::Approx(58.5));
    CHECK(r.cfg.assertMaxKbs == Catch::Approx(150.0));
}

TEST_CASE("parseSwarmArgs parses --server-metrics and --assert-max-tick-ms", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--server-metrics", "/tmp/tick.json", "--assert-max-tick-ms", "16.6"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.serverMetricsPath == "/tmp/tick.json");
    CHECK(r.cfg.assertMaxTickMs == Catch::Approx(16.6));

    const SwarmParseResult bad = parse({"--assert-max-tick-ms", "-1"});
    CHECK(bad.status == ParseStatus::Error);
}

TEST_CASE("parseSwarmArgs parses --assert-min-entities", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--assert-min-entities", "2000"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMinEntities == 2000);

    const SwarmParseResult bad = parse({"--assert-min-entities", "-1"});
    CHECK(bad.status == ParseStatus::Error);
}

TEST_CASE("parseSwarmArgs parses --assert-max-rss-growth-kb (#707)", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--assert-max-rss-growth-kb", "50000"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMaxRssGrowthKb == 50000);

    const SwarmParseResult bad = parse({"--assert-max-rss-growth-kb", "-1"});
    CHECK(bad.status == ParseStatus::Error);
}

TEST_CASE("parseSwarmArgs parses the RSS slope gate flags (#789)", "[bot_swarm][config]") {
    const SwarmParseResult r = parse({"--assert-max-rss-slope-kb-per-min", "25.5", "--rss-sample-interval-s", "10"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMaxRssSlopeKbPerMin == Catch::Approx(25.5));
    CHECK(r.cfg.rssSampleIntervalS == Catch::Approx(10.0));

    CHECK(parse({"--assert-max-rss-slope-kb-per-min", "-1"}).status == ParseStatus::Error);
    CHECK(parse({"--rss-sample-interval-s", "0"}).status == ParseStatus::Error);

    // Defaults: gate disabled, 30 s cadence.
    const SwarmParseResult d = parse({});
    CHECK(d.cfg.assertMaxRssSlopeKbPerMin == Catch::Approx(0.0));
    CHECK(d.cfg.rssSampleIntervalS == Catch::Approx(30.0));
}

TEST_CASE("rssSlopeKbPerMin fits the tail and flags a slow leak (#789)", "[bot_swarm][metrics]") {
    // Realistic shape: RSS climbs to a 128-client plateau in the first 60 s, then flat for the rest.
    std::vector<RssSample> flatTail;
    for (int t = 0; t <= 600; t += 30) {
        const int64_t rss = t < 60 ? 16000 + static_cast<int64_t>(t) * 550 : 49000; // fill, then plateau
        flatTail.push_back({static_cast<double>(t), rss});
    }
    const auto flatSlope = fl::rssSlopeKbPerMin(flatTail, fl::kRssSlopeTailFraction);
    REQUIRE(flatSlope.has_value());
    CHECK(std::abs(*flatSlope) < 1.0); // essentially flat over the tail half

    // A slow leak: still climbing at ~100 KB/min through the whole run.
    std::vector<RssSample> leak;
    for (int t = 0; t <= 600; t += 30)
        leak.push_back({static_cast<double>(t), 16000 + static_cast<int64_t>(t) * 100 / 60}); // 100 KB/min
    const auto leakSlope = fl::rssSlopeKbPerMin(leak, fl::kRssSlopeTailFraction);
    REQUIRE(leakSlope.has_value());
    CHECK(*leakSlope == Catch::Approx(100.0).margin(2.0));

    // Too few points to fit → not evaluable.
    CHECK_FALSE(fl::rssSlopeKbPerMin({}, 0.5).has_value());
    CHECK_FALSE(fl::rssSlopeKbPerMin({{0.0, 16000}}, 0.5).has_value());
}

TEST_CASE("parseSwarmArgs parses the governor asserts with negative-disabled sentinels (#574)", "[bot_swarm][config]") {
    // Defaults are disabled (negative), since 0 is a real value for both.
    const SwarmParseResult d = parse({});
    CHECK(d.cfg.assertMaxLoadFactor < 0.0);
    CHECK(d.cfg.assertMaxDroppedTicks < 0);

    const SwarmParseResult r = parse({"--assert-max-load-factor", "0.99", "--assert-max-dropped-ticks", "0"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.assertMaxLoadFactor == Catch::Approx(0.99));
    CHECK(r.cfg.assertMaxDroppedTicks == 0); // 0 enables the gate (not the disabled sentinel)

    // A missing value is still an error.
    CHECK(parse({"--assert-max-load-factor"}).status == ParseStatus::Error);
    CHECK(parse({"--assert-max-dropped-ticks"}).status == ParseStatus::Error);
}

TEST_CASE("parseSwarmArgs parses the lossy-proxy schedule and congestion asserts (#714)", "[bot_swarm][config]") {
    const SwarmParseResult r =
        parse({"--degrade-duration", "20", "--degrade-start", "15", "--degrade-loss", "0.1", "--degrade-delay-ms",
               "150", "--assert-congestion-engaged-hz", "30", "--assert-congestion-recovered-hz", "55"});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.degradeDurationS == Catch::Approx(20.0));
    CHECK(r.cfg.degradeStartS == Catch::Approx(15.0));
    CHECK(r.cfg.degradeLoss == Catch::Approx(0.1));
    CHECK(r.cfg.degradeDelayMs == 150);
    CHECK(r.cfg.assertCongestionEngagedHz == Catch::Approx(30.0));
    CHECK(r.cfg.assertCongestionRecoveredHz == Catch::Approx(55.0));

    // Defaults: proxy off, asserts disabled.
    const SwarmParseResult d = parse({});
    CHECK(d.cfg.degradeDurationS == Catch::Approx(0.0));
    CHECK(d.cfg.assertCongestionEngagedHz == Catch::Approx(0.0));

    // Validation: loss out of range, negative delay, a degrade window with nothing degraded, and
    // out-of-range assert thresholds are all rejected.
    CHECK(parse({"--degrade-duration", "5", "--degrade-loss", "1.5"}).status == ParseStatus::Error);
    CHECK(parse({"--degrade-delay-ms", "-5"}).status == ParseStatus::Error);
    CHECK(parse({"--degrade-duration", "5"}).status == ParseStatus::Error); // no loss and no delay
    CHECK(parse({"--assert-congestion-engaged-hz", "61"}).status == ParseStatus::Error);
    CHECK(parse({"--assert-congestion-recovered-hz", "-1"}).status == ParseStatus::Error);
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

TEST_CASE("buildReport's slope gate fails a slow leak and passes a plateau (#789)", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.durationS = 600;
    cfg.assertMaxRssSlopeKbPerMin = 20.0; // fail sustained growth above 20 KB/min
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 600.0));

    std::vector<RssSample> plateau, leak;
    for (int t = 0; t <= 600; t += 30) {
        plateau.push_back({static_cast<double>(t), t < 60 ? 16000 + static_cast<int64_t>(t) * 550 : 49000});
        leak.push_back({static_cast<double>(t), 16000 + static_cast<int64_t>(t) * 100 / 60}); // ~100 KB/min
    }

    CHECK(buildReport(cfg, clients, 600.0, {}, 1, std::nullopt, plateau).assertsPassed);
    CHECK_FALSE(buildReport(cfg, clients, 600.0, {}, 1, std::nullopt, leak).assertsPassed);

    // No series while the gate is enabled → not evaluable → fail (cannot pass an unchecked gate).
    CHECK_FALSE(buildReport(cfg, clients, 600.0, {}, 1, std::nullopt, {}).assertsPassed);

    // Gate disabled → passes regardless.
    cfg.assertMaxRssSlopeKbPerMin = 0.0;
    CHECK(buildReport(cfg, clients, 600.0, {}, 1, std::nullopt, leak).assertsPassed);
}

TEST_CASE("reportToJson emits the rss_series and slope (#789)", "[bot_swarm][metrics]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));
    std::vector<RssSample> series = {{0.0, 16000}, {30.0, 20000}, {60.0, 20100}};
    const std::string json = reportToJson(buildReport(cfg, clients, 10.0, {16.6}, 1, std::nullopt, series));

    CHECK(json.find("\"rss_series\"") != std::string::npos);
    CHECK(json.find("\"rss_kb\": 16000") != std::string::npos);
    CHECK(json.find("\"rss_slope_kb_per_min\"") != std::string::npos);
    CHECK(json.find("\"max_rss_slope_kb_per_min\"") != std::string::npos);
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

    CHECK(json.find("\"schema_version\": 4") != std::string::npos); // v4 adds "rss_series" (#789)
    CHECK(json.find("\"observed_server_tick_hz\"") != std::string::npos);
    CHECK(json.find("\"downstream_kbs_per_client\"") != std::string::npos);
    CHECK(json.find("\"clients_connected\": 1") != std::string::npos);
    CHECK(json.find("\"asserts\"") != std::string::npos);
    // No server metrics passed -> no authoritative block.
    CHECK(json.find("\"server_tick\"") == std::string::npos);
}

static ServerTickReport makeServer(double p99) {
    ServerTickReport s;
    s.tickHz = 60.0;
    s.ticksSampled = 600;
    s.peers = 1;
    s.total = {1.0, 2.0, 9.0, 4.0, p99, 0.5};
    s.phases[static_cast<int>(TickPhase::Integrate)] = {0.1, 0.8, 2.0, 1.4, 1.8, 0.2};
    return s;
}

TEST_CASE("reportToJson embeds the server_tick block when server metrics are present",
          "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    const SwarmReport withServer = buildReport(cfg, clients, 10.0, {16.6}, 1, makeServer(6.0));
    const std::string json = reportToJson(withServer);
    CHECK(withServer.hasServer);
    CHECK(json.find("\"server_tick\"") != std::string::npos);
    CHECK(json.find("\"integrate_ms\"") != std::string::npos);
    CHECK(json.find("\"tick_hz\"") != std::string::npos);

    // The embedded block parses back to the same values (one shape on both sides).
    ServerTickReport parsed;
    REQUIRE(fromJson(json, parsed));
    CHECK(parsed.tickHz == Catch::Approx(60.0).margin(1e-3));
}

TEST_CASE("assert-max-tick-ms gates on the authoritative server p99", "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMaxTickMs = 16.6;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    SECTION("passes when server p99 within budget") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, makeServer(6.0)).assertsPassed);
    }
    SECTION("fails when server p99 exceeds budget") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, makeServer(25.0)).assertsPassed);
    }
    SECTION("fails when the assert is enabled but no server metrics were provided") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
}

TEST_CASE("assert-min-entities gates on the authoritative server entity count (#573)",
          "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMinEntities = 2000;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    auto serverWithEntities = [](uint32_t n) {
        ServerTickReport s = makeServer(6.0);
        s.entities = n;
        return s;
    };

    SECTION("passes when the server reached the requested entity count") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithEntities(2050)).assertsPassed);
    }
    SECTION("fails when the server is short of the requested count") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithEntities(128)).assertsPassed);
    }
    SECTION("fails when the assert is enabled but no server metrics were provided") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
}

TEST_CASE("assert-max-rss-growth-kb gates on the server RSS growth (#707)", "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMaxRssGrowthKb = 50000; // allow up to ~50 MB growth
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    auto serverWithRss = [](uint64_t startupKb, uint64_t nowKb) {
        ServerTickReport s = makeServer(6.0);
        s.rssStartupKb = startupKb;
        s.rssKb = nowKb;
        return s;
    };

    SECTION("passes when RSS growth is within the cap") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithRss(200000, 240000)).assertsPassed); // +40 MB
    }
    SECTION("passes when RSS is flat") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithRss(200000, 200000)).assertsPassed);
    }
    SECTION("fails when RSS grew beyond the cap") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithRss(200000, 300000)).assertsPassed); // +100 MB
    }
    SECTION("fails when the assert is enabled but no server metrics were provided") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
}

TEST_CASE("assert-max-load-factor gates on the overrun governor engaging (#574)", "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMaxLoadFactor = 0.99; // require the governor to have shed (load_factor < 1)
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    auto serverWithLoad = [](double lf) {
        ServerTickReport s = makeServer(6.0);
        s.loadFactor = lf;
        return s;
    };

    SECTION("passes when the governor engaged and shed under load") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithLoad(0.70)).assertsPassed);
    }
    SECTION("fails when the governor never engaged (load_factor stayed 1.0)") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithLoad(1.0)).assertsPassed);
    }
    SECTION("fails when the assert is enabled but no server metrics were provided") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
    SECTION("a threshold of exactly 0 is still enabled (negative-disabled sentinel, not zero)") {
        cfg.assertMaxLoadFactor = 0.0;
        // load_factor 0.0 <= 0.0 passes; any positive load_factor fails.
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithLoad(0.0)).assertsPassed);
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithLoad(0.5)).assertsPassed);
    }
    SECTION("a negative threshold disables the gate") {
        cfg.assertMaxLoadFactor = -1.0;
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithLoad(1.0)).assertsPassed);
        CHECK(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed); // no server block, but gate off
    }
}

TEST_CASE("assert-max-dropped-ticks gates on the graceful-not-spiral property (#574)",
          "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMaxDroppedTicks = 0; // graceful: the governor should keep GameLoop drops at zero
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    auto serverWithDrops = [](uint64_t n) {
        ServerTickReport s = makeServer(6.0);
        s.droppedTicks = n;
        return s;
    };

    SECTION("passes when no ticks were dropped") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithDrops(0)).assertsPassed);
    }
    SECTION("fails when the sim spiralled and dropped ticks") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithDrops(42)).assertsPassed);
    }
    SECTION("fails when the assert is enabled but no server metrics were provided") {
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
    SECTION("a negative threshold disables the gate") {
        cfg.assertMaxDroppedTicks = -1;
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithDrops(99)).assertsPassed);
    }
}

TEST_CASE("reportToJson emits the governor assert thresholds (#574)", "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    cfg.assertMaxLoadFactor = 0.99;
    cfg.assertMaxDroppedTicks = 0;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));
    const std::string json = reportToJson(buildReport(cfg, clients, 10.0, {}, 1, makeServer(6.0)));
    CHECK(json.find("\"max_load_factor\"") != std::string::npos);
    CHECK(json.find("\"max_dropped_ticks\"") != std::string::npos);
}

TEST_CASE("congestion gates assert on the run-long server watermarks (#714)", "[bot_swarm][metrics][servertick]") {
    SwarmConfig cfg;
    cfg.clients = 1;
    std::vector<ClientMetrics> clients;
    clients.push_back(makeClient(40000, 0, 600, 0.0, 10.0));

    auto serverWithCongestion = [](double minHz, double recoveredHz) {
        ServerTickReport s = makeServer(6.0);
        s.congestionMinSendHz = minHz;
        s.congestionRecoveredSendHz = recoveredHz;
        return s;
    };

    SECTION("engaged: passes when the controller throttled below the threshold") {
        cfg.assertCongestionEngagedHz = 30.0;
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 60.0)).assertsPassed);
    }
    SECTION("engaged: fails when the controller never engaged (min stayed 60)") {
        cfg.assertCongestionEngagedHz = 30.0;
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(60.0, 60.0)).assertsPassed);
    }
    SECTION("recovered: passes when the rate climbed back above the threshold") {
        cfg.assertCongestionRecoveredHz = 55.0;
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 58.0)).assertsPassed);
    }
    SECTION("recovered: fails when the controller stayed throttled after the window") {
        cfg.assertCongestionRecoveredHz = 55.0;
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 20.0)).assertsPassed);
    }
    SECTION("both gates together express engaged-then-recovered") {
        cfg.assertCongestionEngagedHz = 30.0;
        cfg.assertCongestionRecoveredHz = 55.0;
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 60.0)).assertsPassed);
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(60.0, 60.0)).assertsPassed);
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 30.0)).assertsPassed);
    }
    SECTION("fails when a gate is enabled but no server metrics were provided") {
        cfg.assertCongestionEngagedHz = 30.0;
        CHECK_FALSE(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed);
    }
    SECTION("0 disables both gates") {
        CHECK(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(60.0, 60.0)).assertsPassed);
        CHECK(buildReport(cfg, clients, 10.0, {}, 1).assertsPassed); // no server block, gates off
    }
    SECTION("thresholds appear in the JSON asserts block") {
        cfg.assertCongestionEngagedHz = 30.0;
        cfg.assertCongestionRecoveredHz = 55.0;
        const std::string json = reportToJson(buildReport(cfg, clients, 10.0, {}, 1, serverWithCongestion(12.0, 60.0)));
        CHECK(json.find("\"congestion_engaged_hz\"") != std::string::npos);
        CHECK(json.find("\"congestion_recovered_hz\"") != std::string::npos);
    }
}

// ---- --transport (#649) -------------------------------------------------------------------------

TEST_CASE("transport defaults to enet - bot_swarm stays the enet6 regression instrument (#649)",
          "[bot_swarm][config][transport]") {
    const SwarmParseResult r = parse({});
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.cfg.transport == "enet");
}

TEST_CASE("parseSwarmArgs accepts --transport gns and enet (#649)", "[bot_swarm][config][transport]") {
    const SwarmParseResult g = parse({"--transport", "gns"});
    REQUIRE(g.status == ParseStatus::Ok);
    CHECK(g.cfg.transport == "gns");

    const SwarmParseResult e = parse({"--transport", "enet"});
    REQUIRE(e.status == ParseStatus::Ok);
    CHECK(e.cfg.transport == "enet");
}

TEST_CASE("parseSwarmArgs rejects an unknown transport (#649)", "[bot_swarm][config][transport]") {
    const SwarmParseResult r = parse({"--transport", "quic"});
    CHECK(r.status == ParseStatus::Error);
    CHECK_THAT(r.error, Catch::Matchers::ContainsSubstring("--transport"));
}

TEST_CASE("the report records the transport actually spoken, normalizing the enet6 alias (#649)",
          "[bot_swarm][metrics][transport]") {
    // The gate cross-checks this key: a GNS run that silently fell back to enet6 must not be able
    // to report "gns". bot_swarm refuses that fallback outright, and the report states the truth.
    SwarmConfig cfg;
    cfg.transport = "gns";
    std::vector<ClientMetrics> clients(1);
    clients[0].connected = true;
    const SwarmReport gns = buildReport(cfg, clients, 1.0, {}, 1);
    CHECK(gns.transport == "gns");
    CHECK_THAT(reportToJson(gns), Catch::Matchers::ContainsSubstring("\"transport\": \"gns\""));

    cfg.transport = "enet6"; // accepted alias — normalized so the gate only ever sees enet/gns
    const SwarmReport enet = buildReport(cfg, clients, 1.0, {}, 1);
    CHECK(enet.transport == "enet");
}

TEST_CASE("weapons pattern duty-cycles fire and staggers across the swarm", "[bot_swarm][pattern]") {
    WeaponsPattern p;

    // Over a window, the gun bit is set roughly half the time (a 50% duty cycle) — not never, not
    // always — and store releases fire in brief pulses.
    int gunSet = 0, storeSet = 0, samples = 0;
    for (int i = 0; i < 600; ++i) {
        const double t = static_cast<double>(i) * 0.1; // 60 s at 10 Hz
        const BotControl c = p.sample(t, 3u);
        if (c.buttons & 0x01u)
            ++gunSet;
        if (c.buttons & 0x04u)
            ++storeSet;
        ++samples;
        CHECK(c.throttle >= 0.f);
        CHECK(c.throttle <= 1.f);
    }
    CHECK(gunSet > samples / 4);       // fires often...
    CHECK(gunSet < (samples * 3) / 4); // ...but not constantly
    CHECK(storeSet > 0);               // stores do release
    CHECK(storeSet < samples / 5);     // ...only in brief pulses

    // Staggered: two different clients do not fire their store on the same schedule.
    bool differ = false;
    for (int i = 0; i < 100 && !differ; ++i) {
        const double t = static_cast<double>(i) * 0.05;
        if ((p.sample(t, 0u).buttons & 0x04u) != (p.sample(t, 40u).buttons & 0x04u))
            differ = true;
    }
    CHECK(differ);

    // Deterministic: same (t, client) → same output.
    CHECK(p.sample(1.23, 7u).buttons == p.sample(1.23, 7u).buttons);
}
