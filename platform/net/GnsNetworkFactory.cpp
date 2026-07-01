// SPDX-License-Identifier: GPL-3.0-or-later
#include "GnsNetworkFactory.h"
#include "GnsNetwork.h"

namespace fl {

std::unique_ptr<INetwork> createGnsNetwork() {
    return std::make_unique<GnsNetwork>();
}

const char* gnsLibraryVersion() {
    // GNS does not expose a runtime version string in the standalone flat API; report the pinned tag
    // from cmake/dependencies.cmake.
    return "GameNetworkingSockets 1.6.0";
}

} // namespace fl
