// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The typed repositories behind IPersistence (#533, D24), and the small result type they share.
//
// One repository per domain, each speaking in its own row types rather than in rows and columns.
// #533 declares exactly the one it implements — blobs. IAccountRepository, IStatsRepository and
// IBanRepository are #534's, declared there alongside the schema and the row types that give them
// meaning; declaring them here first would ship an interface with no implementation and nothing to
// test, which is how a HAL becomes a wishlist.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl::persist {

// The outcome of an operation that can fail without it being a programming error: a disk that
// filled, a database a newer build already migrated, a Postgres that went away. Not an exception —
// nothing in this subsystem throws across its own seam, and the server's existing error idiom is
// to report and log.
struct Result {
    bool ok{true};
    std::string error;

    [[nodiscard]] static Result success() {
        return {};
    }
    [[nodiscard]] static Result failure(std::string message) {
        return {false, std::move(message)};
    }
    explicit operator bool() const noexcept {
        return ok;
    }
};

// Opaque byte payloads keyed by an application-chosen string.
//
// This is the ONE untyped repository, and it is untyped on purpose: D25 absorbs the campaign
// `.flsave` as an opaque blob column rather than re-modelling a save format that already has a
// versioned writer and reader of its own. Keys are namespaced by their owner ("campaign/<name>"),
// which keeps one flat table honest without inventing a second schema layer over it.
//
// Reads are synchronous on the calling thread. Writes RETURN ONCE QUEUED — see the threading
// contract in IPersistence.h — so a caller that needs the bytes on disk calls IPersistence::flush()
// afterwards.
class IBlobRepository {
  public:
    virtual ~IBlobRepository() = default;

    // nullopt = no such key. An I/O failure also answers nullopt and logs at Error; the two are
    // distinguishable through StoreHealth for an operator, and not worth distinguishing at a call
    // site that has to handle "no saved campaign" anyway.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> get(std::string_view key) = 0;

    [[nodiscard]] virtual bool exists(std::string_view key) = 0;

    // Every key starting with prefix, sorted. Pass "" for all of them.
    [[nodiscard]] virtual std::vector<std::string> keys(std::string_view prefix) = 0;

    // Insert or replace. Takes the value by value because it outlives this call: it is moved onto
    // the writer thread's queue, and a span or string_view here would be a dangling read later.
    virtual void put(std::string_view key, std::vector<std::byte> value) = 0;

    // Removing an absent key is not an error.
    virtual void remove(std::string_view key) = 0;
};

} // namespace fl::persist
