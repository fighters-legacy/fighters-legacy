// SPDX-License-Identifier: GPL-3.0-or-later
// #358: every SessionFailure enumerator has a stable i18n key that is present + non-empty in the repo's
// locale/en/ui.toml (the enum/TOML drift guard), and the tr() helper falls back to the built-in English
// when there is no locale or the key is missing.

#include "Localize.h"
#include "SessionStatus.h"

#include "ILogger.h"
#include "i18n/Localization.h"
#include "stdfs/StdFilesystem.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <filesystem>

using namespace fl;

namespace {
struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Every failure worth displaying (None is never shown).
constexpr std::array<SessionFailure, 18> kAll{
    SessionFailure::ServerSpawnFailed,   SessionFailure::ServerBindFailed,    SessionFailure::ServerStartTimeout,
    SessionFailure::ServerStartHang,     SessionFailure::VersionMismatch,     SessionFailure::Banned,
    SessionFailure::AccessDenied,        SessionFailure::RateLimited,         SessionFailure::TooManyConnections,
    SessionFailure::ConnectionRefused,   SessionFailure::ConnectTimeout,      SessionFailure::RoleDenied,
    SessionFailure::MissingRequiredPack, SessionFailure::EntitlementRequired, SessionFailure::MatchFull,
    SessionFailure::BadPassword,         SessionFailure::ServerFull,          SessionFailure::NoAirframe,
};
} // namespace

TEST_CASE("SessionFailure keys resolve in en/ui.toml and tr falls back (#358)") {
    NullLog log;
    // FL_REPO_ROOT is the assets root (contains locale/); userData is unused here.
    StdFilesystem fs(std::filesystem::path(FL_REPO_ROOT), std::filesystem::path(FL_REPO_ROOT));
    Localization loc(fs, log);
    REQUIRE(loc.load("en", {}));

    for (SessionFailure f : kAll) {
        const char* key = sessionFailureKey(f);
        INFO(key);
        REQUIRE(std::strlen(key) > 0);                      // every enumerator has a key
        REQUIRE(std::strlen(sessionFailureMessage(f)) > 0); // and a built-in English fallback
        const char* v = loc.get(key);
        CHECK(std::strcmp(v, key) != 0); // present in ui.toml (a miss returns the key itself)
        CHECK(std::strlen(v) > 0);
        CHECK(std::strcmp(tr(&loc, key, "BUILTIN"), v) == 0); // tr uses the localized value
    }

    // None has no key and is never displayed.
    CHECK(std::strlen(sessionFailureKey(SessionFailure::None)) == 0);

    // tr fallback: no locale -> builtin; unknown key -> builtin.
    CHECK(std::strcmp(tr(nullptr, "session.banned", "FALLBACK"), "FALLBACK") == 0);
    CHECK(std::strcmp(tr(&loc, "no.such.key.zzz", "FALLBACK"), "FALLBACK") == 0);
}
