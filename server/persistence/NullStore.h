// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The store an operator gets from `[persistence] enabled = false` (#533).
//
// It exists so callers stay branch-free: a call site that has to write `if (mStore) mStore->...`
// eventually forgets once, and the forgotten branch is a null dereference on a code path only the
// operators who disabled persistence ever run. Every write is a no-op and every read is empty,
// which is exactly the pre-#533 behaviour of a server with no banlist_path configured.
//
// It reports `backendName() == "null"` and `open == false`, so nothing about a disabled store has
// to be inferred: the admin surface says the server is persisting nothing, in those words.

#include "IPersistence.h"

#include <memory>

namespace fl::persist {

[[nodiscard]] std::unique_ptr<IPersistence> makeNullStore();

} // namespace fl::persist
