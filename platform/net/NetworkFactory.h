// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Backend-selecting network factory (#507). Include this instead of any concrete backend
// header (ENetNetwork.h / GnsNetwork.h) so consumers stay HAL-agnostic and never pull enet6
// or GameNetworkingSockets/protobuf headers.
#include "INetwork.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace fl {

class ILogger;

// Selectable transport backend. Enet6 is always available; Gns is compiled only when the
// build was configured with FL_ENABLE_GNS (protobuf + OpenSSL present).
enum class TransportKind : uint8_t { Enet, Gns };

// Creates the requested transport backend. When Gns is requested in an enet6-only build,
// logs a warning (if log != nullptr) and returns the enet6 backend rather than nullptr, so
// callers always get a usable transport.
std::unique_ptr<INetwork> createNetwork(TransportKind kind, ILogger* log = nullptr);

// Human-readable backend version string (e.g. "enet6 6.1.3" / "GameNetworkingSockets 1.6.0").
// Valid for the lifetime of the process. Returns the enet string for Gns in an enet6-only build.
const char* networkBackendVersion(TransportKind kind);

// Parses "enet"/"gns" (case-insensitive) into a TransportKind; returns fallback for anything else.
TransportKind parseTransportKind(std::string_view s, TransportKind fallback);

} // namespace fl
