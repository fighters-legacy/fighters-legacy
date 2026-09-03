// SPDX-License-Identifier: GPL-3.0-or-later
// Bans and the allowlist, backed by the store (#535, D25).
//
// This is Stage 1's acceptance clause: "bans survive restart; a pre-existing banlist.txt imports".
// The import runs exactly once in a deployment's life and has to be right the first time, and the
// file is deliberately LEFT ON DISK afterwards — which makes "the store must win on every later
// load" the property most worth pinning, because getting it wrong silently un-bans people.
#include "AccessRules.h"

#include <IPersistence.h>
#include <NullStore.h>
#include <SqliteStore.h>

#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("fl-access-rules-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string file(const char* name) const {
        return (path / name).string();
    }
};

void writeLines(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::unique_ptr<persist::IPersistence> openStore(const TempDir& dir, ILogger* log) {
    std::string error;
    persist::SqliteOptions opts;
    opts.path = dir.file("fl-server.db");
    auto store = persist::openSqliteStore(opts, log, error);
    INFO("open error: " << error);
    REQUIRE(store);
    return store;
}

constexpr std::int64_t kNow = 1'700'000'000;

} // namespace

TEST_CASE("bans: an empty store and no file yields nothing", "[access_rules]") {
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const auto loaded = loadBanRules({store.get(), ""}, kNow, &log);
    CHECK(loaded.ips.empty());
    CHECK_FALSE(loaded.importedFile);
}

TEST_CASE("bans: a ban survives a close and reopen", "[access_rules]") {
    // The acceptance clause's first half, in miniature.
    TempDir dir;
    NullLogger log;
    {
        auto store = openStore(dir, &log);
        recordBan(store.get(), "203.0.113.7", "console", "griefing", kNow);
        REQUIRE(store->flush().ok);
        store->close();
    }
    auto reopened = openStore(dir, &log);
    const auto loaded = loadBanRules({reopened.get(), ""}, kNow + 5000, &log);
    REQUIRE(loaded.ips.size() == 1u);
    CHECK(loaded.ips.count("203.0.113.7") == 1u);
    CHECK(loaded.fromStore);

    // And the audit trail the flat file never had.
    const auto rules = reopened->bans().all(persist::RuleEffect::Deny);
    REQUIRE(rules.size() == 1u);
    CHECK(rules[0].createdBy == "console");
    CHECK(rules[0].reason == "griefing");
    CHECK(rules[0].expiresAt == 0); // permanent: nothing sets a duration yet, by design
}

TEST_CASE("bans: unbanning removes the rule, and an unknown IP is not an error", "[access_rules]") {
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);

    recordBan(store.get(), "203.0.113.7", "console", "", kNow);
    recordBan(store.get(), "203.0.113.8", "peer 4", "", kNow);
    REQUIRE(store->flush().ok);

    removeBan(store.get(), "203.0.113.7");
    removeBan(store.get(), "198.51.100.99"); // never banned
    REQUIRE(store->flush().ok);
    CHECK(store->health().writesFailed == 0);

    const auto loaded = loadBanRules({store.get(), ""}, kNow, &log);
    REQUIRE(loaded.ips.size() == 1u);
    CHECK(loaded.ips.count("203.0.113.8") == 1u);
}

TEST_CASE("bans: a pre-existing banlist.txt imports once and the file stays", "[access_rules]") {
    // The acceptance clause's second half, and the upgrade path for every server running today.
    TempDir dir;
    RecordingLogger log;
    auto store = openStore(dir, &log);
    const auto banlist = dir.file("banlist.txt");
    writeLines(banlist, "# a comment\n203.0.113.10\n203.0.113.11\n\n203.0.113.12\n");

    const auto first = loadBanRules({store.get(), banlist}, kNow, &log);
    CHECK(first.ips.size() == 3u);
    CHECK(first.importedFile);
    CHECK(log.count(LogLevel::Info, "imported") == 1);
    // Left in place: it is the operator's ban list, and a downgrade should still find it.
    CHECK(std::filesystem::exists(banlist));

    REQUIRE(store->flush().ok);

    // Second load comes from the store and does not import again.
    const auto second = loadBanRules({store.get(), banlist}, kNow, &log);
    CHECK(second.ips.size() == 3u);
    CHECK(second.fromStore);
    CHECK_FALSE(second.importedFile);
    CHECK(log.count(LogLevel::Info, "imported") == 1);

    // The imported rows carry an honest provenance rather than pretending an admin typed them.
    const auto rules = store->bans().all(persist::RuleEffect::Deny);
    REQUIRE(rules.size() == 3u);
    CHECK(rules[0].createdBy == "import");
    CHECK(rules[0].reason.find("imported from") != std::string::npos);
}

TEST_CASE("bans: THE STORE WINS over the file that was imported from", "[access_rules]") {
    // ⚑ The property that makes leaving the file on disk safe. After the import the file is frozen
    // at the moment of the upgrade while the store keeps changing. If a later load preferred the
    // file, every unban since the upgrade would silently come back and every new ban would vanish --
    // and it would look like the server simply forgot.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const auto banlist = dir.file("banlist.txt");
    writeLines(banlist, "203.0.113.10\n203.0.113.11\n");

    REQUIRE(loadBanRules({store.get(), banlist}, kNow, &log).importedFile);
    REQUIRE(store->flush().ok);

    // An operator lifts one ban and adds another, after the import.
    removeBan(store.get(), "203.0.113.10");
    recordBan(store.get(), "203.0.113.99", "console", "", kNow + 100);
    REQUIRE(store->flush().ok);

    const auto loaded = loadBanRules({store.get(), banlist}, kNow + 200, &log);
    CHECK(loaded.fromStore);
    CHECK(loaded.ips.count("203.0.113.10") == 0u); // stayed unbanned
    CHECK(loaded.ips.count("203.0.113.11") == 1u);
    CHECK(loaded.ips.count("203.0.113.99") == 1u); // and the new ban is there
}

TEST_CASE("bans: an emptied store does not resurrect the file", "[access_rules]") {
    // The subtle half of the rule above. Once the store has held rules, unbanning the LAST one
    // leaves it legitimately empty -- and an "empty means fall back to the file" test would
    // re-import every ban the operator just lifted.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const auto banlist = dir.file("banlist.txt");
    writeLines(banlist, "203.0.113.10\n");

    REQUIRE(loadBanRules({store.get(), banlist}, kNow, &log).importedFile);
    REQUIRE(store->flush().ok);
    removeBan(store.get(), "203.0.113.10");
    REQUIRE(store->flush().ok);

    const auto loaded = loadBanRules({store.get(), banlist}, kNow, &log);
    CHECK(loaded.ips.empty());
    CHECK_FALSE(loaded.importedFile);
}

TEST_CASE("bans: a lapsed rule is not applied", "[access_rules]") {
    // Nothing sets a duration yet (#535 scope), but the repository filters expiry and the loader
    // passes the clock through -- so a rule written by a future `--for`, or by hand in psql, is
    // honoured rather than silently permanent.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);

    persist::AccessRule temp;
    temp.effect = persist::RuleEffect::Deny;
    temp.subjectKind = persist::SubjectKind::Ip;
    temp.subject = "203.0.113.20";
    temp.createdAt = kNow;
    temp.expiresAt = kNow + 100;
    store->bans().add(temp);
    recordBan(store.get(), "203.0.113.21", "console", "", kNow); // permanent
    REQUIRE(store->flush().ok);

    CHECK(loadBanRules({store.get(), ""}, kNow + 50, &log).ips.size() == 2u);
    const auto later = loadBanRules({store.get(), ""}, kNow + 500, &log);
    REQUIRE(later.ips.size() == 1u);
    CHECK(later.ips.count("203.0.113.21") == 1u);
}

TEST_CASE("bans: account-keyed rules are skipped, not mistaken for addresses", "[access_rules]") {
    // The schema carries both key kinds from day one (D25), but the connect gauntlet matches on an
    // ADDRESS. Letting an account id into the address set would be a ban that never matches anyone
    // and an allowlist that could refuse everyone.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);

    persist::AccessRule byAccount;
    byAccount.effect = persist::RuleEffect::Deny;
    byAccount.subjectKind = persist::SubjectKind::Account;
    byAccount.subject = "0189d6e4-7c1a-7000-8000-0123456789ab";
    byAccount.realm = "local";
    byAccount.createdAt = kNow;
    store->bans().add(byAccount);
    recordBan(store.get(), "203.0.113.30", "console", "", kNow);
    REQUIRE(store->flush().ok);

    const auto loaded = loadBanRules({store.get(), ""}, kNow, &log);
    REQUIRE(loaded.ips.size() == 1u);
    CHECK(loaded.ips.count("203.0.113.30") == 1u);
    // The rule is still in the record -- it is skipped for matching, not dropped.
    CHECK(store->bans().all(persist::RuleEffect::Deny).size() == 2u);
}

TEST_CASE("allowlist: imports and loads independently of bans", "[access_rules]") {
    // One table, two effects (#534). What must never happen is the two leaking into each other:
    // an allowlist entry that banned someone, or a ban that admitted them.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const auto banlist = dir.file("banlist.txt");
    const auto allowlist = dir.file("allowlist.txt");
    writeLines(banlist, "203.0.113.40\n");
    writeLines(allowlist, "198.51.100.5\n198.51.100.6\n");

    REQUIRE(loadBanRules({store.get(), banlist}, kNow, &log).importedFile);
    REQUIRE(loadAllowRules({store.get(), allowlist}, kNow, &log).importedFile);
    REQUIRE(store->flush().ok);

    const auto bans = loadBanRules({store.get(), banlist}, kNow, &log);
    const auto allows = loadAllowRules({store.get(), allowlist}, kNow, &log);
    REQUIRE(bans.ips.size() == 1u);
    CHECK(bans.ips.count("203.0.113.40") == 1u);
    REQUIRE(allows.ips.size() == 2u);
    CHECK(allows.ips.count("198.51.100.5") == 1u);
    CHECK(allows.ips.count("203.0.113.40") == 0u); // the ban did not leak in
}

TEST_CASE("bans: with persistence disabled nothing is recorded and nothing is imported", "[access_rules]") {
    // A ban still works for the life of the process (the broadcaster holds it), but it must not
    // claim to have been recorded, and the file must not be consumed into a store that discards it.
    TempDir dir;
    NullLogger log;
    auto store = persist::makeNullStore();
    const auto banlist = dir.file("banlist.txt");
    writeLines(banlist, "203.0.113.50\n");

    recordBan(store.get(), "203.0.113.51", "console", "", kNow);
    CHECK(store->flush().ok);

    const auto loaded = loadBanRules({store.get(), banlist}, kNow, &log);
    // The file is still READ -- an operator with persistence off keeps the behaviour they had --
    // but nothing was imported into a store that would drop it.
    CHECK(loaded.ips.count("203.0.113.50") == 1u);
    CHECK_FALSE(loaded.importedFile);
    CHECK_FALSE(loaded.fromStore);
}

TEST_CASE("bans: no store at all behaves like persistence disabled", "[access_rules]") {
    TempDir dir;
    NullLogger log;
    const auto banlist = dir.file("banlist.txt");
    writeLines(banlist, "203.0.113.60\n");

    recordBan(nullptr, "203.0.113.61", "console", "", kNow); // must not crash
    removeBan(nullptr, "203.0.113.61");

    const auto loaded = loadBanRules({nullptr, banlist}, kNow, &log);
    CHECK(loaded.ips.count("203.0.113.60") == 1u);
    CHECK_FALSE(loaded.fromStore);
}
