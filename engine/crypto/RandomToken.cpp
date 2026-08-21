// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto/RandomToken.h"

#include <cstdint>
#include <random>

namespace fl {

std::string randomHexToken(std::size_t chars) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(chars);
    uint32_t word = 0;
    unsigned nibbles = 0;
    while (out.size() < chars) {
        if (nibbles == 0) {
            word = static_cast<uint32_t>(rd());
            nibbles = 8;
        }
        out += kHex[word & 0xFu];
        word >>= 4;
        --nibbles;
    }
    return out;
}

} // namespace fl
