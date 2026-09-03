// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace fl {

// A UUIDv7 (RFC 9562): 48 bits of Unix-milliseconds timestamp, then version/variant bits, then 74
// random bits. Returned canonically — lowercase, hyphenated, 36 characters.
//
// WHY v7 AND NOT v4 (#534, plan #1366 decision D25). Account ids are opaque and globally unique, and
// they are also the PRIMARY KEY of a growing table. A v4 id is uniformly random, so every insert
// lands in a random leaf of the B-tree; a v7 id sorts by creation time, so inserts append and the
// index stays dense. It also makes "the oldest accounts" a range scan rather than a sort, and it
// leaks nothing an account row does not already record in created_at.
//
// The random half is drawn the way randomHexToken draws (see RandomToken.h): every word from
// std::random_device, never a PRNG seeded from one draw. An account id is not a capability, so the
// entropy is not load-bearing the way a token's is, but a seeded-PRNG id would collide across
// processes that started in the same millisecond — which is exactly the case a server restart and a
// test suite both produce.
//
// This lives in engine-crypto rather than in the persistence library because identity (#537/#538)
// mints the same ids, and two generators for one id format is one too many.
[[nodiscard]] std::string uuidv7();

// The same, with the timestamp supplied rather than read from the clock. For tests that need two ids
// a known distance apart, and for any caller that already has the millisecond it means.
[[nodiscard]] std::string uuidv7At(std::uint64_t unixMillis);

// True when `s` is a canonically-formatted UUID: 36 chars, lowercase hex, hyphens at 8/13/18/23.
// Does NOT check the version — a v4 id read back from a store written by an older build is still a
// well-formed id, and this is a format check, not a provenance one.
[[nodiscard]] bool isCanonicalUuid(const std::string& s);

} // namespace fl
