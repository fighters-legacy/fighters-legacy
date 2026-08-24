// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>

namespace fl {

// What the two transport backends agree on, and only they (#1265).
//
// enet6 and GNS differ in almost everything — packet wrapping, channel mapping, peer bookkeeping —
// and those differences are the point of having two backends. The one thing they must NOT differ on
// is what a bind address MEANS, because that string comes straight from the operator's
// `[network].bind_address` and a server that listens on a different interface depending on which
// transport it was built with is an operator-visible bug with no error message attached.
//
// Lives next to NetworkFactory.h in platform/net/ because both backends are here and nothing else
// needs it: the engine never reaches platform-net, and this header does not change that.

// How to interpret an INetwork::bind address, per the contract documented on INetwork::bind:
// "0.0.0.0" (or empty/null) for any IPv4, "::" for dual-stack any, anything else a literal address.
enum class BindAddrKind {
    AnyV4,   // wildcard IPv4 — the platform default when no address is configured
    AnyV6,   // wildcard IPv6: dual-stack on Linux, IPv6-only on Windows
    Literal, // a specific address, parsed by each backend in its own address type
};

[[nodiscard]] inline BindAddrKind classifyBindAddress(const char* address) noexcept {
    if (!address || address[0] == '\0' || std::strcmp(address, "0.0.0.0") == 0)
        return BindAddrKind::AnyV4;
    if (std::strcmp(address, "::") == 0)
        return BindAddrKind::AnyV6;
    return BindAddrKind::Literal;
}

} // namespace fl
