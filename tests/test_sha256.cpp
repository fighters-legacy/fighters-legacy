// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256 (FIPS 180-4) tests (#490): NIST example vectors + streaming/chunk-split invariance.
#include <catch2/catch_test_macros.hpp>

#include "crypto/Sha256.h"

#include <string>
#include <vector>

using namespace fl;

TEST_CASE("SHA-256 matches the NIST example vectors", "[crypto][sha256]") {
    CHECK(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string s56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha256Hex(s56.data(), s56.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 of a million 'a' bytes matches the NIST vector, streamed", "[crypto][sha256]") {
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) // 1,000,000 bytes total, fed in chunks
        h.update(chunk.data(), chunk.size());
    CHECK(sha256Hex(h.finalize()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 is invariant to chunk boundaries", "[crypto][sha256]") {
    std::vector<uint8_t> data(300);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
    const auto oneShot = sha256Hex(data.data(), data.size());

    // Feed the same bytes split at various boundaries (crossing the 64-byte block, 1/63/64/65).
    for (std::size_t step : {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{128}}) {
        Sha256 h;
        for (std::size_t off = 0; off < data.size(); off += step)
            h.update(data.data() + off, std::min(step, data.size() - off));
        CHECK(sha256Hex(h.finalize()) == oneShot);
    }
}

TEST_CASE("SHA-256 reset reuses the object", "[crypto][sha256]") {
    Sha256 h;
    h.update("abc", 3);
    (void)h.finalize();
    h.reset();
    h.update("abc", 3);
    CHECK(sha256Hex(h.finalize()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
