// SPDX-License-Identifier: GPL-3.0-or-later
//
// randomHexToken (#1233) — the one token generator behind the single-player admin token, MCP
// session ids and the pilot GUID. Its contract: exact length, lowercase hex only, and every call
// independent (full entropy per token, no seeded generator whose state one observed token reveals).

#include <crypto/RandomToken.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace fl;

TEST_CASE("randomHexToken: exact length and lowercase-hex charset") {
    for (std::size_t n : {0u, 1u, 7u, 8u, 24u, 32u}) {
        const std::string t = randomHexToken(n);
        CHECK(t.size() == n);
        CHECK(t.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
}

TEST_CASE("randomHexToken: repeated draws are distinct") {
    // 24 hex chars = 96 bits; any collision in a handful of draws means the generator is broken,
    // not unlucky.
    std::set<std::string> seen;
    for (int i = 0; i < 16; ++i)
        seen.insert(randomHexToken(24));
    CHECK(seen.size() == 16);
}
