// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "voice/RadioNet.h"

using namespace fl;

TEST_CASE("radio net kind names round-trip", "[voice]") {
    for (uint8_t i = 0; i <= kMaxRadioNetKind; ++i) {
        const auto kind = static_cast<RadioNetKind>(i);
        RadioNetKind parsed{};
        REQUIRE(radioNetKindFromString(radioNetKindName(kind), parsed));
        REQUIRE(parsed == kind);
    }
    RadioNetKind unused{};
    REQUIRE_FALSE(radioNetKindFromString("frequency", unused));
}

TEST_CASE("radio net kind ordinal guard rejects attacker bytes", "[voice]") {
    REQUIRE(isRadioNetKindOrdinal(0));
    REQUIRE(isRadioNetKindOrdinal(kMaxRadioNetKind));
    REQUIRE_FALSE(isRadioNetKindOrdinal(kMaxRadioNetKind + 1));
    REQUIRE_FALSE(isRadioNetKindOrdinal(255));
}

TEST_CASE("radio net table indexes by insertion order", "[voice]") {
    RadioNetTable t;
    REQUIRE(t.add(RadioNetDef{"team", "TEAM", RadioNetKind::Team}) == 0);
    REQUIRE(t.add(RadioNetDef{"flight", "FLIGHT", RadioNetKind::Flight}) == 1);
    // The index IS the wire netId, so order must be exactly insertion order.
    REQUIRE(t.indexOf("team") == 0);
    REQUIRE(t.indexOf("flight") == 1);
    REQUIRE(t.indexOf("nope") == kInvalidRadioNet);
    REQUIRE(t.byIndex(0)->name == "TEAM");
    REQUIRE(t.byIndex(2) == nullptr);
    REQUIRE(t.byIndex(kInvalidRadioNet) == nullptr);
}

TEST_CASE("radio net table rejects empty and duplicate ids and enforces the cap", "[voice]") {
    RadioNetTable t;
    REQUIRE(t.add(RadioNetDef{"", "X", RadioNetKind::Team}) == kInvalidRadioNet);
    REQUIRE(t.add(RadioNetDef{"a", "A", RadioNetKind::Team}) == 0);
    // Ids are how config and admin commands address a net; a duplicate would make one unreachable.
    REQUIRE(t.add(RadioNetDef{"a", "A2", RadioNetKind::Team}) == kInvalidRadioNet);

    for (std::size_t i = 1; i < kMaxRadioNets; ++i)
        REQUIRE(t.add(RadioNetDef{"n" + std::to_string(i), "", RadioNetKind::Global}) != kInvalidRadioNet);
    REQUIRE(t.size() == kMaxRadioNets);
    REQUIRE(t.add(RadioNetDef{"overflow", "", RadioNetKind::Global}) == kInvalidRadioNet);
}

TEST_CASE("radio net table truncates over-long strings to the wire caps", "[voice]") {
    RadioNetTable t;
    const std::string longId(64, 'i');
    const std::string longName(64, 'n');
    REQUIRE(t.add(RadioNetDef{longId, longName, RadioNetKind::Team}) == 0);
    REQUIRE(t.byIndex(0)->id.size() == kMaxRadioNetIdChars);
    REQUIRE(t.byIndex(0)->name.size() == kMaxRadioNetNameChars);
}

TEST_CASE("radio net table falls back to the id when no display name is given", "[voice]") {
    RadioNetTable t;
    t.add(RadioNetDef{"tanker", "", RadioNetKind::Global});
    REQUIRE(t.byIndex(0)->name == "tanker");
}

TEST_CASE("radio net table default selection prefers the flagged net, then a team net", "[voice]") {
    RadioNetTable empty;
    REQUIRE(empty.defaultIndex() == kInvalidRadioNet);

    RadioNetTable a;
    a.add(RadioNetDef{"global", "ALL", RadioNetKind::Global});
    a.add(RadioNetDef{"team", "TEAM", RadioNetKind::Team});
    REQUIRE(a.defaultIndex() == 1); // no flag: the team net is where a pilot lives

    RadioNetTable b;
    b.add(RadioNetDef{"global", "ALL", RadioNetKind::Global, false, 0.f, true, 1.f, /*defaultNet=*/true});
    b.add(RadioNetDef{"team", "TEAM", RadioNetKind::Team});
    REQUIRE(b.defaultIndex() == 0); // an explicit flag wins

    RadioNetTable c;
    c.add(RadioNetDef{"a", "A", RadioNetKind::Global});
    REQUIRE(c.defaultIndex() == 0); // neither: index 0, never kInvalidRadioNet for a non-empty table
}

TEST_CASE("builtin radio nets give a zero-config server a working radio stack", "[voice]") {
    RadioNetTable t;
    for (auto& def : builtinRadioNets())
        REQUIRE(t.add(def) != kInvalidRadioNet);

    REQUIRE(t.size() == 4);
    REQUIRE(t.indexOf("team") != kInvalidRadioNet);
    REQUIRE(t.indexOf("flight") != kInvalidRadioNet);
    REQUIRE(t.indexOf("atc") != kInvalidRadioNet);
    REQUIRE(t.indexOf("proximity") != kInvalidRadioNet);
    REQUIRE(t.defaultIndex() == t.indexOf("team"));

    // Proximity is the ONLY positional net by default: a radio arrives in your headset, it does not
    // come from the other aircraft's bearing.
    for (const auto& n : t.nets())
        REQUIRE(n.positional == (n.kind == RadioNetKind::Proximity));
    REQUIRE(t.byIndex(t.indexOf("proximity"))->rangeM > 0.f);
}
