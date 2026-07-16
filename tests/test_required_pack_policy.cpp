// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/RequiredPackPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("parseRequiredPackSpec splits id@version and trims (#872)", "[required_packs]") {
    RequiredPack a = parseRequiredPackSpec("fl-base");
    CHECK(a.id == "fl-base");
    CHECK(a.version.empty());

    RequiredPack b = parseRequiredPackSpec("theater@1.2");
    CHECK(b.id == "theater");
    CHECK(b.version == "1.2");

    RequiredPack c = parseRequiredPackSpec("  spaced @ 0.3.1 ");
    CHECK(c.id == "spaced");
    CHECK(c.version == "0.3.1");

    // Only the first '@' splits; the rest is version.
    RequiredPack d = parseRequiredPackSpec("weird@a@b");
    CHECK(d.id == "weird");
    CHECK(d.version == "a@b");
}

TEST_CASE("parseRequiredPackPolicy maps the config strings (#872)", "[required_packs]") {
    CHECK(parseRequiredPackPolicy("warn") == RequiredPackPolicy::Warn);
    CHECK(parseRequiredPackPolicy("refuse") == RequiredPackPolicy::Refuse);
    CHECK(parseRequiredPackPolicy("allow_placeholder") == RequiredPackPolicy::AllowPlaceholder);
    CHECK(parseRequiredPackPolicy("allow-placeholder") == RequiredPackPolicy::AllowPlaceholder);
    CHECK(parseRequiredPackPolicy("nonsense") == std::nullopt);
    CHECK(parseRequiredPackPolicy("") == std::nullopt);
}

TEST_CASE("requiredPackPolicyName round-trips (#872)", "[required_packs]") {
    CHECK(parseRequiredPackPolicy(requiredPackPolicyName(RequiredPackPolicy::Warn)) == RequiredPackPolicy::Warn);
    CHECK(parseRequiredPackPolicy(requiredPackPolicyName(RequiredPackPolicy::Refuse)) == RequiredPackPolicy::Refuse);
    CHECK(parseRequiredPackPolicy(requiredPackPolicyName(RequiredPackPolicy::AllowPlaceholder)) ==
          RequiredPackPolicy::AllowPlaceholder);
}

TEST_CASE("missingRequiredPacks flags absent ids (#872)", "[required_packs]") {
    std::vector<RequiredPack> required = {RequiredPack{"fl-base"}, RequiredPack{"theater"}};
    std::vector<ClientPack> client = {{"fl-base", "0.3.1"}};

    auto missing = missingRequiredPacks(required, client);
    REQUIRE(missing.size() == 1u);
    CHECK(missing[0] == "theater");
}

TEST_CASE("missingRequiredPacks is satisfied when every id is present, version unpinned (#872)", "[required_packs]") {
    std::vector<RequiredPack> required = {RequiredPack{"fl-base"}};
    std::vector<ClientPack> client = {{"fl-base", "9.9.9"}, {"extra", "1.0"}};
    CHECK(missingRequiredPacks(required, client).empty());
}

TEST_CASE("missingRequiredPacks enforces a pinned version and reports id@version (#872)", "[required_packs]") {
    std::vector<RequiredPack> required = {RequiredPack{"fl-base", "1.2"}};

    // Version matches -> satisfied.
    CHECK(missingRequiredPacks(required, {{"fl-base", "1.2"}}).empty());

    // Version mismatch -> missing, reported with the required version so the user knows what to install.
    auto missing = missingRequiredPacks(required, {{"fl-base", "1.1"}});
    REQUIRE(missing.size() == 1u);
    CHECK(missing[0] == "fl-base@1.2");

    // Absent entirely -> also missing, same annotation.
    auto absent = missingRequiredPacks(required, {{"other", "1.2"}});
    REQUIRE(absent.size() == 1u);
    CHECK(absent[0] == "fl-base@1.2");
}

TEST_CASE("missingRequiredPacks with no requirements is always satisfied (#872)", "[required_packs]") {
    CHECK(missingRequiredPacks({}, {}).empty());
    CHECK(missingRequiredPacks({}, {{"fl-base", "1.0"}}).empty());
}
