// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header for the GameNetworkingSockets backend. Include this (never GnsNetwork.h)
// so consumers are not exposed to GNS/protobuf headers. Only available when FL_ENABLE_GNS is set.
#include "INetwork.h"
#include <memory>

namespace fl {

std::unique_ptr<INetwork> createGnsNetwork();

// Human-readable library version string (e.g. "GameNetworkingSockets 1.6.0").
// Valid for the lifetime of the process.
const char* gnsLibraryVersion();

} // namespace fl
