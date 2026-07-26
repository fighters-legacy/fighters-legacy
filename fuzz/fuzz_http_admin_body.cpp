// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the JSON request-body scanners behind fl-server's REST admin API
// (fl::httpadmin::jsonNumberField / jsonStringField). These read the bodies of POST /kick, /ban,
// /unban and /shutdown -- bytes that arrive from the network before any command runs -- so they are
// exactly the parse surface #233 puts in front of an authenticated attacker.
//
// cpp-httplib owns HTTP framing itself; what is OURS is this field extraction, and it must fail
// closed on every shape: unterminated strings, a trailing backslash at end-of-buffer, a key with no
// colon, an over-long value, embedded NULs.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <HttpAdminServer.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view body(reinterpret_cast<const char*>(data), size);

    // The four keys the real routes read, plus one that is never present, so the not-found path is
    // driven as hard as the found path.
    for (const char* key : {"peer", "ip", "in", "reason", "absent"}) {
        (void)fl::httpadmin::jsonNumberField(body, key);
        (void)fl::httpadmin::jsonStringField(body, key);
    }

    // The credential path takes attacker-controlled header bytes too.
    (void)fl::httpadmin::extractBearer(body);
    return 0;
}
