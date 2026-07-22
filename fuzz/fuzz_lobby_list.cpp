// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness for parseLobbyServerList (#143): the tolerant lobby server-list JSON scanner. It
// must never read out of bounds or over-allocate on hostile input.
#include "net/LobbyListClient.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)fl::parseLobbyServerList(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
