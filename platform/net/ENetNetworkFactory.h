// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header. Include this instead of ENetNetwork.h so consumers are
// never exposed to enet6 headers.
#include "INetwork.h"
#include <memory>

namespace fl {

std::unique_ptr<INetwork> createENetNetwork();

// Returns a human-readable library version string (e.g. "enet6 6.1.3").
// Valid for the lifetime of the process.
const char* enetLibraryVersion();

} // namespace fl
