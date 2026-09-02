// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// [persistence] -> an opened store, or a refusal (#533).
//
// This lives outside ServerRuntime for one reason: it decides WHETHER THE SERVER STARTS, and a
// decision that consequential should be reachable by a test. As a private method on
// ServerRuntime::Impl it was not — Impl is a translation-unit-local type reached only by running a
// whole server, so the postgres branch and the refusal path were exercised by nothing.
//
// It is also the seam cmake/layering.cmake rule 8 draws. fl-server-persistence may not reach
// fl-server-lib: the store takes plain option structs and knows nothing about ServerConfig, which is
// what lets it be tested without standing up a server. Something has to map one onto the other, and
// this is that something — on the fl-server side of the line, where ServerConfig lives.

#include "server_config.h"

#include <memory>
#include <string>

namespace fl {

class ILogger;

namespace persist {
class IPersistence;
}

// The outcome of trying to satisfy [persistence].
struct PersistenceOpenResult {
    // Non-null on success. `enabled = false` yields the null store, which is a SUCCESS: the operator
    // asked for no persistence and got exactly that.
    std::unique_ptr<persist::IPersistence> store;
    // Empty on success. On failure this is the whole operator-facing message, already naming the
    // backend, the underlying reason and the `enabled = false` escape hatch — the caller logs it and
    // refuses to start rather than composing its own.
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return store != nullptr;
    }
};

// Open whatever [persistence] describes. Never throws; a null store always comes with an error.
[[nodiscard]] PersistenceOpenResult openConfiguredStore(const ServerConfig& cfg, ILogger* log);

} // namespace fl
