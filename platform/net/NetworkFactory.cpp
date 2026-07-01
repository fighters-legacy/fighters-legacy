// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetworkFactory.h"

#include "ENetNetworkFactory.h"
#include "ILogger.h"

#include <cctype>
#include <string>

#ifdef FL_ENABLE_GNS
#include "GnsNetworkFactory.h"
#endif

namespace fl {

std::unique_ptr<INetwork> createNetwork(TransportKind kind, ILogger* log) {
    if (kind == TransportKind::Gns) {
#ifdef FL_ENABLE_GNS
        (void)log; // only the enet6-only fallback path logs a warning
        return createGnsNetwork();
#else
        if (log)
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "GNS transport requested but this build is enet6-only (FL_ENABLE_GNS=OFF); "
                     "falling back to enet6");
        return createENetNetwork();
#endif
    }
    return createENetNetwork();
}

const char* networkBackendVersion(TransportKind kind) {
    if (kind == TransportKind::Gns) {
#ifdef FL_ENABLE_GNS
        return gnsLibraryVersion();
#else
        return enetLibraryVersion();
#endif
    }
    return enetLibraryVersion();
}

TransportKind parseTransportKind(std::string_view s, TransportKind fallback) {
    std::string lower(s);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "gns")
        return TransportKind::Gns;
    if (lower == "enet" || lower == "enet6")
        return TransportKind::Enet;
    return fallback;
}

} // namespace fl
