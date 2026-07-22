// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "i18n/Localization.h"

#include <cstring>

namespace fl {

// The single translate helper (#358): look `key` up in the active locale, falling back to the built-in
// English `builtin` when there is no locale or the key is absent. Localization::get() returns the key
// itself on a miss, so a returned string equal to the key means "not translated" → use the builtin.
// A null/empty key returns the builtin unchanged.
inline const char* tr(const Localization* loc, const char* key, const char* builtin) {
    if (!loc || !key || !*key)
        return builtin;
    const char* v = loc->get(key);
    return (v && std::strcmp(v, key) != 0) ? v : builtin;
}

} // namespace fl
