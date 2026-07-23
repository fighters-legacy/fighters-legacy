// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The single mod-manifest (manifest.toml) schema owner (#651). ModLoader::parseManifest and
// validate-mod both go through parseModManifest so the loader and the validator cannot drift on the
// required-field / identifier rules — the anti-drift contract the mission/campaign parsers hold.

#include "content/TrustLevel.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The engine-api major version this build accepts. A manifest's `engine-api` major must equal it.
inline constexpr const char* kEngineApiMajor = "1";

struct ModManifestInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string engineApi;
    std::string namespaceId; // `[mod] namespace`; defaults to id
    int priority{0};
    std::vector<std::string> depends;
    TrustLevel trustLevel{TrustLevel::Unsigned};
    bool hasSignature{false}; // [mod.trust].signature present (GPG verification is Phase 6)
};

struct ModManifestParseResult {
    bool ok{true};
    ModManifestInfo manifest;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Parse + validate a manifest.toml's contents, accumulating ALL errors (never fail-fast). Checks the
// required [mod] fields (name/id/version/engine-api/priority), sanitizes id/name/namespace as safe
// path identifiers, and reads the optional depends[] + [mod.trust].
[[nodiscard]] ModManifestParseResult parseModManifest(std::string_view tomlContent);

// True when `engineApi`'s major component equals kEngineApiMajor (e.g. "1.0"/"1.4" ok, "2.0" not).
[[nodiscard]] bool engineApiCompatible(const std::string& engineApi);

} // namespace fl
