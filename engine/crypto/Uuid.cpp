// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto/Uuid.h"

#include <chrono>
#include <cstddef>
#include <random>

namespace fl {
namespace {

constexpr char kHex[] = "0123456789abcdef";

void appendHexByte(std::string& out, std::uint8_t b) {
    out += kHex[(b >> 4) & 0xFu];
    out += kHex[b & 0xFu];
}

} // namespace

std::string uuidv7At(std::uint64_t unixMillis) {
    std::uint8_t b[16]{};

    // Bytes 0-5: the 48-bit timestamp, big-endian. Big-endian is what makes the CANONICAL STRING
    // sort in time order, which is the whole point -- a little-endian layout would still be a valid
    // v7 by the bit definitions and would sort as noise, and the store orders on the text.
    b[0] = static_cast<std::uint8_t>((unixMillis >> 40) & 0xFF);
    b[1] = static_cast<std::uint8_t>((unixMillis >> 32) & 0xFF);
    b[2] = static_cast<std::uint8_t>((unixMillis >> 24) & 0xFF);
    b[3] = static_cast<std::uint8_t>((unixMillis >> 16) & 0xFF);
    b[4] = static_cast<std::uint8_t>((unixMillis >> 8) & 0xFF);
    b[5] = static_cast<std::uint8_t>(unixMillis & 0xFF);

    // Bytes 6-15: random. Every 32-bit word straight from random_device -- see the header.
    std::random_device rd;
    std::uint32_t word = 0;
    unsigned bytesLeft = 0;
    for (std::size_t i = 6; i < 16; ++i) {
        if (bytesLeft == 0) {
            word = static_cast<std::uint32_t>(rd());
            bytesLeft = 4;
        }
        b[i] = static_cast<std::uint8_t>(word & 0xFFu);
        word >>= 8;
        --bytesLeft;
    }

    // Version 7 in the high nibble of byte 6, RFC 4122 variant (0b10) in the top bits of byte 8.
    // Written AFTER the random fill, so the fill cannot clobber them.
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x70);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);

    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        appendHexByte(out, b[i]);
    }
    return out;
}

std::string uuidv7() {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return uuidv7At(static_cast<std::uint64_t>(ms));
}

bool isCanonicalUuid(const std::string& s) {
    if (s.size() != 36)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

} // namespace fl
