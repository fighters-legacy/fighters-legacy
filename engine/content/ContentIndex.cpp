// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ContentIndex.h"

#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/IContentPack.h"

#include <toml++/toml.hpp>

#include <cctype>
#include <cstdint>
#include <string>

namespace fl {
namespace {

// Every def TOML puts its id under a single root table, and the three agree on the key name:
// [entity] id / [sensor] id / [weapon] id. Shallow-parsing that one key is what keeps engine-content
// UNDER engine-entity / engine-sensor / engine-weapon in the layer graph — the index must not link
// the full parsers to learn a def's id.
const char* rootTableFor(AssetType type) noexcept {
    switch (type) {
    case AssetType::EntityDef:
        return "entity";
    case AssetType::SensorDef:
        return "sensor";
    case AssetType::Weapon:
        return "weapon";
    default:
        return nullptr; // not a def type — has no id
    }
}

std::string lowered(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string indexKey(AssetType type, std::string_view id) {
    return std::to_string(static_cast<uint8_t>(type)) + ':' + lowered(id);
}

// An id feeds a path builder the moment it is resolved to an asset name, so a separator in one is a
// path-traversal boundary, not a style violation. Mirrors ModLoader::isValidIdentifier's rejections.
bool isPathSafeId(std::string_view id) noexcept {
    return !id.empty() && id.find('/') == std::string_view::npos && id.find('\\') == std::string_view::npos &&
           id.find('\0') == std::string_view::npos;
}

} // namespace

void ContentIndex::build(AssetManager& assets, std::span<const AssetType> types, ILogger& log) {
    m_idToAsset.clear();

    for (AssetType type : types) {
        const char* root = rootTableFor(type);
        if (!root) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    ("content index: asset type " + std::to_string(static_cast<uint8_t>(type)) +
                     " has no def id and cannot be indexed")
                        .c_str());
            continue;
        }

        // listAssets() returns names in pack-priority order, so "first id wins" here IS
        // "highest-priority pack wins" — the same precedence every other content lookup obeys.
        for (const std::string& assetName : assets.listAssets(type)) {
            std::shared_ptr<AssetBase> raw = assets.loadDefBytes(type, assetName.c_str());
            if (!raw || raw->bytes.empty()) {
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        ("content index: '" + assetName + "' could not be loaded; not indexed").c_str());
                continue;
            }

            std::string id;
            try {
                const toml::table tbl =
                    toml::parse(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
                if (auto v = tbl[root]["id"].value<std::string>())
                    id = std::move(*v);
            } catch (const toml::parse_error& e) {
                // The full parser will report this properly when the def is actually loaded; here it
                // just means the def has no reachable id, so it cannot be cross-referenced.
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        ("content index: '" + assetName + "' is not parseable TOML: " + e.what()).c_str());
                continue;
            }

            if (id.empty()) {
                log.log(
                    LogLevel::Warn, __FILE__, __LINE__,
                    ("content index: '" + assetName + "' declares no [" + root + "] id; it cannot be cross-referenced")
                        .c_str());
                continue;
            }

            if (!isPathSafeId(id)) {
                log.log(LogLevel::Error, __FILE__, __LINE__,
                        ("content index: def id '" + id + "' in '" + assetName +
                         "' contains a path separator; refusing to index it")
                            .c_str());
                continue;
            }

            const auto colon = id.find(':');
            if (colon == std::string::npos) {
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        ("content index: def id '" + id + "' in '" + assetName +
                         "' is not namespaced; expected \"<namespace>:<name>\"")
                            .c_str());
            } else if (const IContentPack* owner = assets.findPackForAsset(type, assetName.c_str())) {
                const std::string_view declared{owner->namespaceId()};
                if (std::string_view(id).substr(0, colon) != declared) {
                    log.log(LogLevel::Warn, __FILE__, __LINE__,
                            ("content index: def id '" + id + "' in '" + assetName +
                             "' does not match its pack's declared namespace '" + std::string(declared) + "'")
                                .c_str());
                }
            }

            auto [it, inserted] = m_idToAsset.emplace(indexKey(type, id), assetName);
            if (!inserted) {
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        ("content index: duplicate def id '" + id + "' in '" + assetName + "'; keeping '" + it->second +
                         "' from the higher-priority pack")
                            .c_str());
            }
        }
    }
}

const std::string* ContentIndex::assetNameFor(AssetType type, std::string_view id) const noexcept {
    auto it = m_idToAsset.find(indexKey(type, id));
    return it == m_idToAsset.end() ? nullptr : &it->second;
}

std::vector<std::string> ContentIndex::idsOfType(AssetType type) const {
    const std::string prefix = std::to_string(static_cast<uint8_t>(type)) + ':';
    std::vector<std::string> ids;
    for (const auto& [key, name] : m_idToAsset) {
        if (key.compare(0, prefix.size(), prefix) == 0)
            ids.push_back(key.substr(prefix.size()));
    }
    return ids;
}

void ContentIndex::clear() noexcept {
    m_idToAsset.clear();
}

} // namespace fl
