// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AlertSystem.h"
#include "world/EscalationPolicyParser.h"
#include "world/FactionRegistry.h"
#include "world/ZoneGeometry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace fl;

namespace {

constexpr double kDt = 1.0 / 60.0;

// The world origin is the north pole and the planet centre is at {0, -R, 0}, so a point at world
// (x, 0, z) sits at altitude ~0 only near the origin. Tests place entities close to the origin and
// use the y component for altitude, which is what geodeticAltitude reports there.
[[nodiscard]] ZoneEntitySample sample(uint32_t idx, uint16_t faction, double x, double y, double z) {
    ZoneEntitySample s;
    s.entityIdx = idx;
    s.entityGen = 1;
    s.factionIndex = faction;
    s.pos = glm::dvec3(x, y, z);
    return s;
}

// Two mutually hostile sides plus the reserved neutral at index 0, matching what applyMission builds.
void loadSides(FactionRegistry& reg) {
    std::vector<FactionDef> defs;
    defs.push_back(FactionDef{"", "(neutral)"});
    defs.push_back(FactionDef{"nato", "NATO"});
    defs.push_back(FactionDef{"russia", "Russia"});
    reg.load(std::move(defs));
    reg.setRelationship(1, 2, FactionRelation::Hostile);
}

[[nodiscard]] AirspaceZone circleZone(std::string id, std::string owner, double radius = 5000.0) {
    AirspaceZone z;
    z.id = std::move(id);
    z.shape = ZoneShape::Circle;
    z.centerX = 0.0;
    z.centerZ = 0.0;
    z.radiusM = radius;
    z.altFloorM = 0.0;
    z.altCeilingM = 12000.0;
    z.ownerFactionId = std::move(owner);
    z.policyId = "test";
    return z;
}

// A deterministic policy with distinct thresholds per stage, so a test can assert exactly which
// stage a given dwell produces.
[[nodiscard]] EscalationPolicy testPolicy() {
    EscalationPolicy p;
    p.id = "test";
    p.name = "Test";
    p.byLevel[static_cast<std::size_t>(AlertLevel::Peacetime)] = {10, 20, 30, true, 5};
    p.byLevel[static_cast<std::size_t>(AlertLevel::Elevated)] = {2, 4, 6, true, 1};
    p.byLevel[static_cast<std::size_t>(AlertLevel::Conflict)] = {1, 2, 3, false, 0};
    p.byLevel[static_cast<std::size_t>(AlertLevel::WarState)] = {0, 0, 0, false, 0};
    return p;
}

// Run `seconds` of ticks. The sample list reaches the system through its sampler, which the tests
// wire to capture their local vector by reference -- so mutating that vector between run() calls is
// how a test flies an aircraft out of a zone.
void run(AlertSystem& sys, double seconds, uint64_t& tick) {
    const int steps = static_cast<int>(seconds / kDt + 0.5);
    for (int i = 0; i < steps; ++i)
        sys.onTick(kDt, tick++);
}

} // namespace

// ── geometry ────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("ZoneGeometry: circle containment", "[alert_system][geometry]") {
    CHECK(pointInCircleXZ(0, 0, 100, 0, 0));
    CHECK(pointInCircleXZ(0, 0, 100, 99, 0));
    CHECK_FALSE(pointInCircleXZ(0, 0, 100, 100, 0)); // boundary is exclusive
    CHECK_FALSE(pointInCircleXZ(0, 0, 100, 71, 71)); // ~100.4 m diagonal
    CHECK_FALSE(pointInCircleXZ(0, 0, 0, 0, 0));     // zero radius contains nothing
    CHECK_FALSE(pointInCircleXZ(0, 0, -5, 0, 0));    // negative radius contains nothing
}

TEST_CASE("ZoneGeometry: polygon containment by ray casting", "[alert_system][geometry]") {
    const std::vector<std::pair<double, double>> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(pointInPolygonXZ(square, 5, 5));
    CHECK_FALSE(pointInPolygonXZ(square, 15, 5));
    CHECK_FALSE(pointInPolygonXZ(square, -1, 5));
    CHECK_FALSE(pointInPolygonXZ(square, 5, 20));

    // A point level with a vertex must not flip parity twice (the half-open edge rule).
    const std::vector<std::pair<double, double>> diamond{{0, 0}, {10, -10}, {20, 0}, {10, 10}};
    CHECK(pointInPolygonXZ(diamond, 10, 0));
    CHECK_FALSE(pointInPolygonXZ(diamond, 25, 0));

    CHECK_FALSE(pointInPolygonXZ({}, 0, 0));
    CHECK_FALSE(pointInPolygonXZ({{0, 0}, {1, 1}}, 0, 0)); // fewer than 3 vertices
}

TEST_CASE("ZoneGeometry: convexity check", "[alert_system][geometry]") {
    CHECK(isConvexPolygonXZ({{0, 0}, {10, 0}, {10, 10}, {0, 10}}));
    CHECK(isConvexPolygonXZ({{0, 0}, {0, 10}, {10, 10}, {10, 0}}));               // opposite winding
    CHECK(isConvexPolygonXZ({{0, 0}, {5, 0}, {10, 0}, {10, 10}, {0, 10}}));       // collinear run is fine
    CHECK_FALSE(isConvexPolygonXZ({{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}})); // concave notch
    CHECK_FALSE(isConvexPolygonXZ({{0, 0}, {10, 10}, {20, 20}}));                 // degenerate line
    CHECK_FALSE(isConvexPolygonXZ({{0, 0}, {1, 1}}));
}

TEST_CASE("ZoneGeometry: altitude band is inclusive at both edges", "[alert_system][geometry]") {
    AirspaceZone z = circleZone("z", "russia");
    z.altFloorM = 1000;
    z.altCeilingM = 5000;
    CHECK(altitudeInZone(z, 1000));
    CHECK(altitudeInZone(z, 5000));
    CHECK(altitudeInZone(z, 3000));
    CHECK_FALSE(altitudeInZone(z, 999));
    CHECK_FALSE(altitudeInZone(z, 5001));

    // zoneContains gates on BOTH shape and altitude.
    CHECK(zoneContains(z, 100, 100, 3000));
    CHECK_FALSE(zoneContains(z, 100, 100, 100));  // inside the circle, below the floor
    CHECK_FALSE(zoneContains(z, 99999, 0, 3000)); // in the band, outside the circle
}

// ── escalation state machine ────────────────────────────────────────────────────────────────────

TEST_CASE("AlertSystem: an intruder escalates through every stage on dwell", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    // NATO faction (index 1) against a russia-owned zone. Make them non-hostile so dwell decides,
    // rather than the belligerent shortcut.
    reg.setRelationship(1, 2, FactionRelation::Neutral);

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    std::vector<EscalationStage> seen;
    sys.onEscalate = [&](uint32_t, const std::string&, EscalationStage s) { seen.push_back(s); };

    uint64_t tick = 0;
    run(sys, 5.0, tick); // below the 10 s warning threshold
    CHECK(sys.isInZone(7, "capital"));
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::InZone);

    run(sys, 6.0, tick); // past 10 s
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);

    run(sys, 10.0, tick); // past 20 s
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Intercept);

    run(sys, 10.0, tick); // past 30 s
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Hostile);

    // Every stage was reported once, in ascending order.
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == EscalationStage::InZone);
    CHECK(seen[1] == EscalationStage::Warned);
    CHECK(seen[2] == EscalationStage::Intercept);
    CHECK(seen[3] == EscalationStage::Hostile);
}

TEST_CASE("AlertSystem: an entity outside the zone never escalates", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 50000, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    int escalations = 0;
    sys.onEscalate = [&](uint32_t, const std::string&, EscalationStage) { ++escalations; };

    uint64_t tick = 0;
    run(sys, 60.0, tick);
    CHECK(escalations == 0);
    CHECK_FALSE(sys.isInZone(7, "capital"));
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Clean);
}

TEST_CASE("AlertSystem: a zone owner is not an intruder in its own airspace", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 2, 0, 5000, 0)}; // russia = index 2 = the owner
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    int escalations = 0;
    sys.onEscalate = [&](uint32_t, const std::string&, EscalationStage) { ++escalations; };

    uint64_t tick = 0;
    run(sys, 60.0, tick);
    CHECK(escalations == 0);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Clean);
}

TEST_CASE("AlertSystem: a belligerent is weapons-free on entry regardless of dwell", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg); // nato/russia are Hostile
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    sys.onTick(kDt, tick++);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Hostile);
}

TEST_CASE("AlertSystem: war_state escalates on entry with no warning", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    reg.setAlertLevel(2, AlertLevel::WarState);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    sys.onTick(kDt, tick++);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Hostile);
}

TEST_CASE("AlertSystem: the owner's alert level selects the dwell row", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    reg.setAlertLevel(2, AlertLevel::Elevated); // warning at 2 s instead of 10 s
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 3.0, tick);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);
}

TEST_CASE("AlertSystem: complying resets the intruder after the cooldown", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy()); // peacetime: complianceReset, 5 s cooldown
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    int exits = 0;
    sys.onZoneExit = [&](uint32_t, const std::string&) { ++exits; };

    uint64_t tick = 0;
    run(sys, 12.0, tick);
    REQUIRE(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);

    // Turn back.
    samples[0].pos = glm::dvec3(50000, 5000, 0);
    run(sys, 1.0, tick);
    CHECK(exits == 1);
    CHECK_FALSE(sys.isInZone(7, "capital"));
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Warned); // stage held during cooldown

    run(sys, 5.0, tick);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Clean); // forgotten
}

TEST_CASE("AlertSystem: without complianceReset the stage sticks after leaving", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    reg.setAlertLevel(2, AlertLevel::Conflict); // conflict row: complianceReset = false
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 1.5, tick);
    REQUIRE(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);

    samples[0].pos = glm::dvec3(50000, 5000, 0);
    run(sys, 30.0, tick);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);
}

TEST_CASE("AlertSystem: lowering the alert level does not revoke a stage already reached", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    reg.setAlertLevel(2, AlertLevel::Elevated);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 7.0, tick); // elevated: past the 6 s hostile threshold
    REQUIRE(sys.getIntruderStage(7, "capital") == EscalationStage::Hostile);

    reg.setAlertLevel(2, AlertLevel::Peacetime); // peacetime would put 7 s at Clean/InZone
    run(sys, 1.0, tick);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Hostile);
}

TEST_CASE("AlertSystem: a recycled pool slot is a new intruder", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 12.0, tick);
    REQUIRE(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);

    samples[0].entityGen = 2; // same index, different aircraft
    sys.onTick(kDt, tick++);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::InZone);
}

TEST_CASE("AlertSystem: a despawned entity's record is pruned", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 12.0, tick);
    REQUIRE(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);

    samples.clear(); // killed
    sys.onTick(kDt, tick++);
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Clean);
}

TEST_CASE("AlertSystem: a zone with an unresolvable owner enforces nothing and is reported", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    AlertSystem sys(reg);
    sys.addPolicy(testPolicy());
    sys.addZone(circleZone("ghost", "atlantis"));

    CHECK(sys.unresolvedZoneIds() == std::vector<std::string>{"ghost"});

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    int escalations = 0;
    sys.onEscalate = [&](uint32_t, const std::string&, EscalationStage) { ++escalations; };

    uint64_t tick = 0;
    run(sys, 60.0, tick);
    CHECK(escalations == 0);
}

TEST_CASE("AlertSystem: a zone naming an unknown policy falls back to the builtin default, not inert",
          "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    reg.setRelationship(1, 2, FactionRelation::Neutral);
    AlertSystem sys(reg);
    // No addPolicy at all -- the zone's "test" policy id resolves to nothing.
    sys.addZone(circleZone("capital", "russia"));

    std::vector<ZoneEntitySample> samples{sample(7, 1, 0, 5000, 0)};
    sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

    uint64_t tick = 0;
    run(sys, 50.0, tick); // default peacetime warns at 45 s
    CHECK(sys.getIntruderStage(7, "capital") == EscalationStage::Warned);
}

TEST_CASE("AlertSystem: the pass is independent of sample order", "[alert_system][determinism]") {
    auto stagesAfter = [](bool reversed) {
        FactionRegistry reg;
        loadSides(reg);
        reg.setRelationship(1, 2, FactionRelation::Neutral);
        AlertSystem sys(reg);
        sys.addPolicy(testPolicy());
        sys.addZone(circleZone("capital", "russia"));
        sys.addZone(circleZone("inner", "russia", 1000.0));

        std::vector<ZoneEntitySample> samples{
            sample(1, 1, 0, 5000, 0),    // inside both
            sample(2, 1, 3000, 5000, 0), // inside capital only
            sample(3, 1, 90000, 5000, 0) // outside both
        };
        if (reversed)
            std::reverse(samples.begin(), samples.end());
        sys.setEntitySampler([&](std::vector<ZoneEntitySample>& out) { out = samples; });

        uint64_t tick = 0;
        run(sys, 25.0, tick);
        return std::vector<EscalationStage>{sys.getIntruderStage(1, "capital"), sys.getIntruderStage(1, "inner"),
                                            sys.getIntruderStage(2, "capital"), sys.getIntruderStage(2, "inner"),
                                            sys.getIntruderStage(3, "capital"), sys.getIntruderStage(3, "inner")};
    };

    CHECK(stagesAfter(false) == stagesAfter(true));
}

TEST_CASE("AlertSystem: setAlertLevel fires the change hook once, and never for a no-op write", "[alert_system]") {
    FactionRegistry reg;
    loadSides(reg);
    AlertSystem sys(reg);

    std::vector<std::pair<uint16_t, AlertLevel>> changes;
    sys.onAlertLevelChange = [&](uint16_t fi, AlertLevel lvl) { changes.emplace_back(fi, lvl); };

    sys.setAlertLevel("russia", AlertLevel::Conflict);
    CHECK(sys.getAlertLevel("russia") == AlertLevel::Conflict);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].first == 2);
    CHECK(changes[0].second == AlertLevel::Conflict);

    sys.setAlertLevel("russia", AlertLevel::Conflict); // same value
    CHECK(changes.size() == 1);

    sys.setAlertLevel("atlantis", AlertLevel::WarState); // unknown faction
    CHECK(changes.size() == 1);
    CHECK(sys.getAlertLevel("atlantis") == AlertLevel::Peacetime);
}

// ── escalation-policy TOML ──────────────────────────────────────────────────────────────────────

TEST_CASE("parseEscalationPolicy: a full policy round-trips", "[alert_system][policy]") {
    const EscalationPolicy p = parseEscalationPolicy(R"toml(
[policy]
id   = "military_intercept"
name = "Standard Military Intercept"

[escalation.peacetime]
warning_dwell       = 45
intercept_dwell     = 90
hostile_dwell       = 180
compliance_reset    = true
compliance_cooldown = 300

[escalation.war_state]
warning_dwell       = 0
intercept_dwell     = 0
hostile_dwell       = 0
compliance_reset    = false
compliance_cooldown = 0
)toml");

    CHECK(p.id == "military_intercept");
    CHECK(p.name == "Standard Military Intercept");

    const EscalationDwell& peace = p.forLevel(AlertLevel::Peacetime);
    CHECK(peace.warningDwellS == 45);
    CHECK(peace.interceptDwellS == 90);
    CHECK(peace.hostileDwellS == 180);
    CHECK(peace.complianceReset);
    CHECK(peace.complianceCooldownS == 300);

    const EscalationDwell& war = p.forLevel(AlertLevel::WarState);
    CHECK(war.warningDwellS == 0);
    CHECK_FALSE(war.complianceReset);

    // An unlisted level keeps the struct defaults.
    CHECK(p.forLevel(AlertLevel::Conflict).warningDwellS == 0);
}

TEST_CASE("parseEscalationPolicy: rejects malformed documents", "[alert_system][policy]") {
    CHECK_THROWS(parseEscalationPolicy("{{{ not toml"));
    CHECK_THROWS(parseEscalationPolicy("[escalation.peacetime]\nwarning_dwell = 1\n")); // no [policy]
    CHECK_THROWS(parseEscalationPolicy("[policy]\nname = \"x\"\n"));                    // no id
    CHECK_THROWS(parseEscalationPolicy("[policy]\nid = \"x\"\n"));                      // no name

    // Unknown level section -- a typo must not silently leave the real row at its defaults.
    CHECK_THROWS(parseEscalationPolicy(R"toml(
[policy]
id = "x"
name = "X"
[escalation.wartime]
warning_dwell = 1
)toml"));

    // Decreasing thresholds make a stage unreachable.
    CHECK_THROWS(parseEscalationPolicy(R"toml(
[policy]
id = "x"
name = "X"
[escalation.peacetime]
warning_dwell   = 90
intercept_dwell = 45
hostile_dwell   = 180
)toml"));

    // Negative dwell.
    CHECK_THROWS(parseEscalationPolicy(R"toml(
[policy]
id = "x"
name = "X"
[escalation.peacetime]
warning_dwell = -5
)toml"));
}

TEST_CASE("alert-level vocabulary round-trips and gates untrusted ordinals", "[alert_system]") {
    for (auto lvl : {AlertLevel::Peacetime, AlertLevel::Elevated, AlertLevel::Conflict, AlertLevel::WarState}) {
        AlertLevel back{};
        REQUIRE(alertLevelFromString(alertLevelName(lvl), back));
        CHECK(back == lvl);
    }
    AlertLevel unused{};
    CHECK_FALSE(alertLevelFromString("wartime", unused));

    CHECK(isAlertLevelOrdinal(0));
    CHECK(isAlertLevelOrdinal(3));
    CHECK_FALSE(isAlertLevelOrdinal(4));
    CHECK(isEscalationStageOrdinal(4));
    CHECK_FALSE(isEscalationStageOrdinal(5));
    CHECK(escalationStageName(EscalationStage::Intercept) == "intercept");
}
