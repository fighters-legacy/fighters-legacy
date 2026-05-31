// SPDX-License-Identifier: GPL-3.0-or-later
#include "ENetNetworkFactory.h"
#include "ENetNetwork.h"
#include <cstdio>
#include <enet6/enet.h>

std::unique_ptr<INetwork> createENetNetwork() {
    return std::make_unique<ENetNetwork>();
}

const char* enetLibraryVersion() {
    static char kBuf[32];
    static bool kReady = false;
    if (!kReady) {
        std::snprintf(kBuf, sizeof(kBuf), "enet6 %d.%d.%d", ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
        kReady = true;
    }
    return kBuf;
}
