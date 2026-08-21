// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::fnv1a64 / fl::fnv1a32 (#1247).
//
// Five sites hand-rolled this hash and two of them used a basis one digit short of FNV-1a's, so
// they computed something that shared a name with FNV-1a but not a value. Nothing recorded the
// truncation as deliberate; it was a typo that propagated by copy.
//
// The guard against it coming back is not "the implementation agrees with itself" -- the truncated
// copies agreed with themselves perfectly for months. It is agreement with the PUBLISHED FNV-1a
// vectors, checked below, which is also the property the planned offline fl-review verifier needs:
// an independent implementation written from the spec has to reproduce these numbers exactly.

#include "math/Fnv.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

TEST_CASE("fnv1a64 reproduces the published FNV-1a 64 vectors", "[fnv]") {
    CHECK(fl::fnv1a64("") == 0xcbf29ce484222325ull);
    CHECK(fl::fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    CHECK(fl::fnv1a64("foobar") == 0x85944171f73967e8ull);

    // The empty input is the basis, which is the definition of the basis.
    CHECK(fl::fnv1a64("") == fl::kFnv1a64Basis);
    CHECK(fl::kFnv1a64Basis == 14695981039346656037ull);
    CHECK(fl::kFnv1a64Prime == 1099511628211ull);
}

TEST_CASE("fnv1a32 reproduces the published FNV-1a 32 vectors", "[fnv]") {
    const auto h = [](std::string_view s) { return fl::fnv1a32(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
    CHECK(h("") == 0x811c9dc5u);
    CHECK(h("a") == 0xe40c292cu);
    CHECK(h("foobar") == 0xbf9cf968u);

    CHECK(fl::kFnv1a32Basis == 2166136261u);
    CHECK(fl::kFnv1a32Prime == 16777619u);
}

TEST_CASE("the basis is not the truncated one this replaced", "[fnv]") {
    // 1469598103934665603 is 14695981039346656037 with its last digit dropped. It was in the tree
    // as `kFnv1a64Offset`, under a comment saying FNV-1a. Naming it here means a future copy-paste
    // of the wrong number fails a test that says why, instead of silently re-forking the hash.
    constexpr uint64_t kTruncatedTypo = 1469598103934665603ull;
    CHECK(fl::kFnv1a64Basis != kTruncatedTypo);
    CHECK(fl::kFnv1a64Basis / 10ull == kTruncatedTypo); // how the typo happened
    CHECK(fl::fnv1a64("campaign") != fl::fnv1a64("campaign", kTruncatedTypo));
}

TEST_CASE("the running value continues one hash rather than combining two", "[fnv]") {
    // airportSourceHash depends on this: it folds two CSVs into ONE hash, so that either file
    // changing changes the result. Hashing them separately and XOR-ing would not have that property.
    CHECK(fl::fnv1a64("bar", fl::fnv1a64("foo")) == fl::fnv1a64("foobar"));
    CHECK(fl::fnv1a64("", fl::fnv1a64("foo")) == fl::fnv1a64("foo"));

    // And it is order-sensitive, which is the point of a fingerprint over a concatenation.
    CHECK(fl::fnv1a64("bar", fl::fnv1a64("foo")) != fl::fnv1a64("foo", fl::fnv1a64("bar")));
}

TEST_CASE("fnv1a64Fold uses its own byte order, not the host's", "[fnv]") {
    // A state hash that folded a uint64 by memcpy would compare two honest recordings from
    // different-endian hosts and call them different. This spells little-endian out.
    uint64_t h = fl::kFnv1a64Basis;
    fl::fnv1a64Fold(h, 0x0807060504030201ull);

    // Typed explicitly: a braced list of bare integer literals is a std::initializer_list<int>, and
    // narrowing it to uint8_t in the loop variable is a /W4 warning MSVC turns into an error.
    constexpr uint8_t kLittleEndianBytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t expected = fl::kFnv1a64Basis;
    for (uint8_t b : kLittleEndianBytes)
        fl::fnv1a64Byte(expected, b);
    CHECK(h == expected);

    // Distinct inputs stay distinct through the fold, including the zero the sentinel-free path
    // relies on.
    uint64_t a = fl::kFnv1a64Basis, b = fl::kFnv1a64Basis;
    fl::fnv1a64Fold(a, 0);
    fl::fnv1a64Fold(b, 1);
    CHECK(a != b);
    CHECK(a != fl::kFnv1a64Basis);
}

TEST_CASE("fnv1a64 is usable at compile time", "[fnv]") {
    // constexpr is not decoration here: it lets a call site fold a fixed name into a constant, and
    // it forbids any future implementation that reaches for a runtime detail like memcpy.
    static_assert(fl::fnv1a64("foobar") == 0x85944171f73967e8ull);
    STATIC_REQUIRE(fl::fnv1a64("") == fl::kFnv1a64Basis);
}
