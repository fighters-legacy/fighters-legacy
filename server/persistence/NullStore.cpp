// SPDX-License-Identifier: GPL-3.0-or-later
#include "NullStore.h"

#include <crypto/Uuid.h>

namespace fl::persist {
namespace {

// Every repository answers "nothing is here" and accepts every write into the void. Callers stay
// branch-free (see NullStore.h); nothing about a disabled store is inferred, because backendName()
// says "null" and health().open is false.
class NullStore final : public IPersistence {
  public:
    IBlobRepository& blobs() override {
        return mBlobs;
    }
    IAccountRepository& accounts() override {
        return mAccounts;
    }
    IBanRepository& bans() override {
        return mBans;
    }
    IStatsRepository& stats() override {
        return mStats;
    }

    Result flush() override {
        return Result::success();
    }
    void close() override {}
    [[nodiscard]] StoreHealth health() const override {
        return StoreHealth{}; // open = false, every counter zero: nothing was ever promised
    }
    [[nodiscard]] std::string_view backendName() const override {
        return "null";
    }

  private:
    struct Blobs final : IBlobRepository {
        std::optional<std::vector<std::byte>> get(std::string_view) override {
            return std::nullopt;
        }
        bool exists(std::string_view) override {
            return false;
        }
        std::vector<std::string> keys(std::string_view) override {
            return {};
        }
        void put(std::string_view, std::vector<std::byte>) override {}
        void remove(std::string_view) override {}
    };

    struct Accounts final : IAccountRepository {
        // Still mints a real id and returns a coherent record. A caller that creates an account and
        // uses its id for the rest of the session works identically with persistence off -- it
        // simply finds nothing on the next run, which is exactly what `enabled = false` promises.
        AccountRecord create(std::string_view realm, std::string_view displayName) override {
            AccountRecord rec;
            rec.id = fl::uuidv7();
            rec.realm = std::string(realm);
            rec.displayName = std::string(displayName);
            return rec;
        }
        std::optional<AccountRecord> get(std::string_view) override {
            return std::nullopt;
        }
        std::optional<AccountRecord> findByName(std::string_view, std::string_view) override {
            return std::nullopt;
        }
        void touchLastSeen(std::string_view, std::int64_t) override {}
        void remove(std::string_view) override {}
    };

    struct Bans final : IBanRepository {
        void add(const AccessRule&) override {}
        void remove(RuleEffect, SubjectKind, std::string_view) override {}
        std::vector<AccessRule> active(RuleEffect, std::int64_t) override {
            return {};
        }
        std::vector<AccessRule> all(RuleEffect) override {
            return {};
        }
    };

    struct Stats final : IStatsRepository {
        std::optional<PilotLogbook> get(std::string_view) override {
            return std::nullopt;
        }
        void put(std::string_view, const PilotLogbook&) override {}
    };

    Blobs mBlobs;
    Accounts mAccounts;
    Bans mBans;
    Stats mStats;
};

} // namespace

std::unique_ptr<IPersistence> makeNullStore() {
    return std::make_unique<NullStore>();
}

} // namespace fl::persist
