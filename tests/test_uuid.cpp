// SPDX-License-Identifier: GPL-3.0-or-later
// UUIDv7 (#534, D25) — the account-id mint shared by persistence and, later, identity (#537/#538).
//
// The property worth testing is the one v7 exists for and v4 does not have: ids sort in creation
// order as TEXT. The store's primary key is that text, so a layout that is a valid v7 by the bit
// definitions but sorts as noise would pass a version-bits check and still lose the whole benefit.
#include "crypto/Uuid.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace fl;

TEST_CASE("uuidv7: canonical shape, version and variant bits", "[uuid]") {
    const std::string id = uuidv7();
    INFO("id = " << id);

    REQUIRE(id.size() == 36);
    CHECK(isCanonicalUuid(id));
    CHECK(id[8] == '-');
    CHECK(id[13] == '-');
    CHECK(id[18] == '-');
    CHECK(id[23] == '-');

    // Version 7 is the first nibble of the third group; the variant is the top bits of the fourth,
    // which in hex means one of 8/9/a/b.
    CHECK(id[14] == '7');
    const char variant = id[19];
    CHECK((variant == '8' || variant == '9' || variant == 'a' || variant == 'b'));
}

TEST_CASE("uuidv7: ids sort in time order as plain text", "[uuid]") {
    // The reason for v7 over v4. Written against uuidv7At so it is a statement about the LAYOUT and
    // not a race with the wall clock: a test that generated two ids and hoped the millisecond
    // ticked between them would be flaky on a fast machine and would prove nothing on a slow one.
    const std::string early = uuidv7At(1'000'000'000'000ull); // 2001-09-09
    const std::string late = uuidv7At(1'900'000'000'000ull);  // 2030-03-17
    CHECK(early < late);

    // Adjacent milliseconds too, since that is the resolution the ordering actually has.
    CHECK(uuidv7At(1'700'000'000'000ull) < uuidv7At(1'700'000'000'001ull));

    // A whole ascending sequence stays sorted, which is what makes an index append rather than
    // scatter.
    std::vector<std::string> ids;
    for (std::uint64_t ms = 1'700'000'000'000ull; ms < 1'700'000'000'050ull; ++ms)
        ids.push_back(uuidv7At(ms));
    CHECK(std::is_sorted(ids.begin(), ids.end()));
}

TEST_CASE("uuidv7: the timestamp is actually the supplied millisecond", "[uuid]") {
    // Byte-exact, because an off-by-one in the shift table would still produce sorted ids and would
    // silently put every account 256 ms or 65 s from where it belongs.
    const std::string id = uuidv7At(0x0123456789ABull);
    CHECK(id.substr(0, 8) == "01234567");
    CHECK(id.substr(9, 4) == "89ab");
}

TEST_CASE("uuidv7: ids minted in the same millisecond do not collide", "[uuid]") {
    // The case a PRNG seeded from one random_device draw would fail: a server restart and a test
    // suite both mint several ids inside one millisecond.
    std::set<std::string> seen;
    constexpr int kCount = 2000;
    for (int i = 0; i < kCount; ++i)
        seen.insert(uuidv7At(1'700'000'000'000ull));
    CHECK(seen.size() == static_cast<std::size_t>(kCount));
}

TEST_CASE("isCanonicalUuid: rejects what a store must not accept as a key", "[uuid]") {
    CHECK(isCanonicalUuid(uuidv7()));
    CHECK_FALSE(isCanonicalUuid(""));
    CHECK_FALSE(isCanonicalUuid("not-a-uuid"));
    // Right length, wrong content.
    CHECK_FALSE(isCanonicalUuid(std::string(36, 'x')));
    // Uppercase is not canonical: two spellings of one id would be two rows.
    CHECK_FALSE(isCanonicalUuid("0189D6E4-7C1A-7000-8000-0123456789AB"));
    // Hyphens in the wrong places.
    CHECK_FALSE(isCanonicalUuid("0189d6e4-7c1a-7000-8000-0123456789ab "));
    CHECK_FALSE(isCanonicalUuid("0189d6e47c1a-7000-8000-0123456789abc"));
}
