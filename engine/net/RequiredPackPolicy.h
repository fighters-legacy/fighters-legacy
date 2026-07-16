// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Required-pack policy (#872): the SERVER side of basic multiplayer content consistency. The connect
// handshake (#853) already carries the client's mounted-pack manifest on the wire (PackManifestEntry);
// this header owns the pure POLICY that compares that manifest against the server's required set and
// decides what to do about a mismatch. It is stdlib-only and header-only so it lives in engine-net
// without touching the zero-dependency wire protocol, and its comparison core is unit-testable in
// isolation (no WorldBroadcaster, no sockets).
namespace fl {

// What the server does when a connecting client is missing a required pack.
enum class RequiredPackPolicy : uint8_t {
    Warn = 0,             // log server-side + notify the client, but ADMIT (today's default behavior)
    Refuse = 1,           // disconnect with MsgConnectRefusal(MissingRequiredPack) + the missing list
    AllowPlaceholder = 2, // silently admit and serve placeholders (the old silent fallback, made explicit)
};

// One pack the server requires. version empty = any version of this id is accepted.
struct RequiredPack {
    std::string id;
    std::string version;

    RequiredPack() = default;
    // Non-explicit so a plain id string still brace-initializes a RequiredPack (config/test ergonomics).
    RequiredPack(std::string packId, std::string packVersion = {})
        : id(std::move(packId)), version(std::move(packVersion)) {}
    // Direct const char* ctor: `{"fl-base"}` from a string literal would otherwise need two user-defined
    // conversions (const char* -> std::string -> RequiredPack), which the language forbids.
    RequiredPack(const char* packId) : id(packId) {}
};

// One pack the client reports having mounted (decoded from its PackManifestEntry manifest).
struct ClientPack {
    std::string id;
    std::string version;
};

// Parse a config spec: "id" or "id@version" (split on the first '@'; surrounding whitespace trimmed).
inline RequiredPack parseRequiredPackSpec(std::string_view spec) {
    auto trim = [](std::string_view s) -> std::string_view {
        const char* ws = " \t\r\n";
        const std::size_t b = s.find_first_not_of(ws);
        if (b == std::string_view::npos)
            return {};
        const std::size_t e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    };
    spec = trim(spec);
    const std::size_t at = spec.find('@');
    if (at == std::string_view::npos)
        return RequiredPack{std::string(spec)};
    return RequiredPack{std::string(trim(spec.substr(0, at))), std::string(trim(spec.substr(at + 1)))};
}

// Map a config policy string to the enum. Accepts "warn", "refuse", and "allow_placeholder"
// (also "allow-placeholder"); nullopt on anything else so the caller can warn + fall back.
inline std::optional<RequiredPackPolicy> parseRequiredPackPolicy(std::string_view s) {
    if (s == "warn")
        return RequiredPackPolicy::Warn;
    if (s == "refuse")
        return RequiredPackPolicy::Refuse;
    if (s == "allow_placeholder" || s == "allow-placeholder")
        return RequiredPackPolicy::AllowPlaceholder;
    return std::nullopt;
}

inline const char* requiredPackPolicyName(RequiredPackPolicy p) noexcept {
    switch (p) {
    case RequiredPackPolicy::Warn:
        return "warn";
    case RequiredPackPolicy::Refuse:
        return "refuse";
    case RequiredPackPolicy::AllowPlaceholder:
        return "allow_placeholder";
    }
    return "warn";
}

// The comparison core: return the required packs the client is MISSING. A pack is missing when its id
// is absent from the client's manifest, OR when a specific version is required and the client's mounted
// version differs. Each returned string is "id" (id-only requirement) or "id@version" (version pinned)
// so the message tells the user exactly what to install. Order follows `required`.
inline std::vector<std::string> missingRequiredPacks(const std::vector<RequiredPack>& required,
                                                     const std::vector<ClientPack>& client) {
    std::vector<std::string> missing;
    for (const RequiredPack& r : required) {
        const ClientPack* found = nullptr;
        for (const ClientPack& c : client) {
            if (c.id == r.id) {
                found = &c;
                break;
            }
        }
        const bool satisfied = found != nullptr && (r.version.empty() || found->version == r.version);
        if (!satisfied)
            missing.push_back(r.version.empty() ? r.id : (r.id + "@" + r.version));
    }
    return missing;
}

} // namespace fl
