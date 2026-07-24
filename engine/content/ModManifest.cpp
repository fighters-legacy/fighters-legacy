// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ModManifest.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <toml++/toml.hpp>

namespace fl {

namespace {
bool isWindowsReservedName(std::string_view component) {
    // CON, PRN, AUX, NUL, COM1-9, LPT1-9 (case-insensitive), optionally with an extension.
    std::string base;
    for (char c : component) {
        if (c == '.')
            break;
        base += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL"};
    for (const char* r : kReserved)
        if (base == r)
            return true;
    if ((base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base.size() == 4 && base[3] >= '1' &&
        base[3] <= '9')
        return true;
    return false;
}

bool isValidIdentifier(std::string_view field) {
    if (field.empty() || field.size() > 128)
        return false;
    if (field.find('\0') != std::string_view::npos)
        return false;
    if (field.find('/') != std::string_view::npos || field.find('\\') != std::string_view::npos)
        return false;
    if (field.size() >= 2 && std::isalpha(static_cast<unsigned char>(field[0])) && field[1] == ':')
        return false; // drive-letter prefix
    if (isWindowsReservedName(field))
        return false;
    return true;
}
} // namespace

bool engineApiCompatible(const std::string& engineApi) {
    auto dot = engineApi.find('.');
    const std::string major = (dot != std::string::npos) ? engineApi.substr(0, dot) : engineApi;
    return major == kEngineApiMajor;
}

ModManifestParseResult parseModManifest(std::string_view tomlContent) {
    ModManifestParseResult r;
    toml::table tbl;
    try {
        tbl = toml::parse(tomlContent);
    } catch (const toml::parse_error& e) {
        r.errors.push_back(std::string("failed to parse manifest: ") + e.description().data());
        r.ok = false;
        return r;
    }

    auto mod = tbl["mod"];
    if (!mod.is_table()) {
        r.errors.push_back("missing [mod] table");
        r.ok = false;
        return r;
    }

    auto name = mod["name"].value<std::string>();
    auto id = mod["id"].value<std::string>();
    auto version = mod["version"].value<std::string>();
    auto engineApi = mod["engine-api"].value<std::string>();
    auto priority = mod["priority"].value<int64_t>();
    if (!name || !id || !version || !engineApi || !priority) {
        r.errors.push_back("missing required field(s) (name, id, version, engine-api, priority)");
        r.ok = false;
        return r;
    }

    ModManifestInfo& m = r.manifest;
    m.name = std::move(*name);
    m.id = std::move(*id);
    m.version = std::move(*version);
    m.engineApi = std::move(*engineApi);
    m.priority = static_cast<int>(*priority);

    if (!isValidIdentifier(m.id))
        r.errors.push_back("invalid id field '" + m.id + "'");
    if (!isValidIdentifier(m.name))
        r.errors.push_back("invalid name field '" + m.name + "'");
    if (!engineApiCompatible(m.engineApi))
        r.errors.push_back("engine-api '" + m.engineApi + "' is incompatible (engine major " + kEngineApiMajor + ")");

    m.namespaceId = mod["namespace"].value<std::string>().value_or(m.id);
    if (!isValidIdentifier(m.namespaceId) || m.namespaceId.find(':') != std::string::npos)
        r.errors.push_back("invalid namespace field '" + m.namespaceId + "'");

    if (auto deps = mod["depends"].as_array())
        for (auto& dep : *deps)
            if (auto s = dep.value<std::string>())
                m.depends.push_back(std::move(*s));

    if (auto trust = tbl["mod"]["trust"]) {
        if (auto sig = trust["signature"].value<std::string>(); sig && !sig->empty())
            m.hasSignature = true;
        if (auto signedBy = trust["signed-by"].value<std::string>()) {
            if (*signedBy == "community")
                m.trustLevel = TrustLevel::Community;
            else if (*signedBy == "maintainer")
                m.trustLevel = TrustLevel::Maintainer;
            else
                r.warnings.push_back("unknown signed-by value '" + *signedBy + "' — defaulting to Unsigned");
        }
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace fl
