// SPDX-License-Identifier: GPL-3.0-or-later
#include "HttpAdminServer.h"

#include <util/Json.h> // json::escape — the one escaper for every JSON this server emits

#include <cctype>
#include <cstdlib>
#include <string>

// The pure half of the HTTP admin surface (#233), in its OWN translation unit.
//
// The header always said this: "everything that makes a security decision lives here so it is
// unit-testable without opening a file descriptor and fuzzable without a network". The DECLARATIONS
// were separated; the definitions still sat in the file that includes httplib.h, so linking the
// token table meant linking an HTTP server. #601 made that concrete — McpEndpoint needs issuerFor
// and links no httplib — so the split the header promised is now real.

namespace fl::httpadmin {

bool buildTokenTable(const ServerConfig::HttpAdminConfig& cfg, std::string_view defaultAutonomy,
                     std::vector<TokenGrant>& out, std::string& error) {
    out.clear();
    // Resolve the fallback tier once. A typo here would otherwise be inherited silently by every row
    // that does not override it, which is the widest possible blast radius for a one-word mistake.
    const auto fallback = mcp::parseAutonomy(defaultAutonomy);
    if (!fallback) {
        error = "unknown ai.mcp.autonomy '" + std::string(defaultAutonomy) + "' (expected observe|recommend|act)";
        return false;
    }
    for (const ServerConfig::HttpAdminToken& row : cfg.tokens) {
        const auto caps = parseRolePreset(row.role);
        if (!caps) {
            error = "unknown role preset '" + row.role + "' (expected admin|moderator|gm|faction_leader)";
            return false;
        }
        TokenGrant g;
        g.token = row.token;
        g.caps = *caps;
        g.role = row.role;
        g.factionIndex = row.faction >= 0 && row.faction <= 0xFFFE ? static_cast<uint16_t>(row.faction)
                                                                   : PeerAuthority::kNoFactionBinding;
        if (row.autonomy.empty()) {
            g.autonomy = *fallback;
        } else if (const auto a = mcp::parseAutonomy(row.autonomy)) {
            g.autonomy = *a;
        } else {
            // Same rule as the role preset: refuse rather than downgrade. A token that silently fell
            // back to `observe` would present as "MCP is broken" rather than "your config is wrong".
            error = "unknown autonomy '" + row.autonomy + "' on token role '" + row.role +
                    "' (expected observe|recommend|act)";
            return false;
        }
        out.push_back(std::move(g));
    }
    return true;
}

std::string extractBearer(std::string_view authHeader) {
    constexpr std::string_view kScheme = "bearer";
    if (authHeader.size() <= kScheme.size())
        return {};
    for (std::size_t i = 0; i < kScheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(authHeader[i])) != kScheme[i])
            return {};
    std::size_t pos = kScheme.size();
    // Require at least one space, then skip any run of them.
    if (authHeader[pos] != ' ')
        return {};
    while (pos < authHeader.size() && authHeader[pos] == ' ')
        ++pos;
    return std::string(authHeader.substr(pos));
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    // Fold the length difference into the accumulator instead of returning early, so the comparison
    // takes the same shape whatever the presented credential looks like.
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

const TokenGrant* resolveToken(const std::vector<TokenGrant>& table, std::string_view presented) {
    if (presented.empty())
        return nullptr;
    const TokenGrant* found = nullptr;
    for (const TokenGrant& g : table)
        if (constantTimeEquals(g.token, presented))
            found = &g; // no break: every row is compared, so timing does not reveal which matched
    return found;
}

CommandIssuer issuerFor(const TokenGrant& grant) noexcept {
    // Built in one expression on purpose. A default-constructed CommandIssuer is ADMIN, so any code
    // path that fills fields one at a time can forget `caps` and silently grant everything.
    return CommandIssuer{kIssuerNoPeer, grant.caps, grant.factionIndex};
}

// Untrusted REST bodies, read through the engine's one JSON scanner (#1080). The old pair located a
// key with find("\"key\""), which matched a key nested inside a sub-object or one that merely appeared
// inside a string VALUE -- `{"note": "\"admin\": true"}` satisfied a lookup for `admin`. json::member
// walks the object and reads keys at its top level only, so these endpoints now accept exactly the flat
// object they document.
//
// kMaxField is deliberately far below json::kMaxStringValue: a kick reason or a token is short, and an
// unterminated quote must not make the server return a megabyte because someone sent one.
namespace {
constexpr std::size_t kMaxField = 512;
} // namespace

std::optional<double> jsonNumberField(std::string_view json, std::string_view key) {
    return fl::json::numberField(json, key);
}

std::optional<std::string> jsonStringField(std::string_view json, std::string_view key) {
    return fl::json::stringField(json, key, kMaxField);
}

std::string errorJson(std::string_view message) {
    return "{\"error\": \"" + json::escape(message) + "\"}";
}

} // namespace fl::httpadmin
