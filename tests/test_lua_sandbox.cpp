// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaSandbox.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace fl;

// All tests use an empty packRootDir — the custom require loader will reject
// all requires (in-pack file not found), which exercises the error path.
// Tests that need in-pack require create a temporary directory with a Lua file.

static std::unique_ptr<LuaSandbox> makeSandbox(const std::string& root = {}) {
    auto sb = LuaSandbox::create(root);
    REQUIRE(sb != nullptr);
    return sb;
}

// ---------------------------------------------------------------------------
// Basic execution
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: valid Lua script executes successfully") {
    auto sb = makeSandbox();
    CHECK(sb->loadScript("local x = 1 + 1"));
    CHECK(sb->lastError().empty());
}

// ---------------------------------------------------------------------------
// Bytecode rejection
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: precompiled Lua bytecode is rejected") {
    auto sb = makeSandbox();
    // First byte \x1b is the Lua bytecode magic
    bool ok = sb->loadScript(std::string_view("\x1bLua", 4));
    CHECK_FALSE(ok);
    CHECK(sb->lastError().find("bytecode") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Deny-list: dangerous libraries / globals
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: io library is not accessible") {
    auto sb = makeSandbox();
    bool ok = sb->loadScript("return io ~= nil");
    // The script itself may succeed (returning false), OR io is nil and the
    // script errors — either way io must not be a valid table.
    if (ok) {
        // If script ran, io was nil (returned false means io == nil)
        // Verify by checking that accessing io.open errors
        auto sb2 = makeSandbox();
        CHECK_FALSE(sb2->loadScript("io.open('test', 'r')"));
    }
    // If script failed, io was nil and indexing it caused an error — also good.
}

TEST_CASE("LuaSandbox: os library is not accessible") {
    auto sb = makeSandbox();
    CHECK_FALSE(sb->loadScript("os.execute('echo hi')"));
}

TEST_CASE("LuaSandbox: debug library is not accessible") {
    auto sb = makeSandbox();
    CHECK_FALSE(sb->loadScript("debug.traceback()"));
}

TEST_CASE("LuaSandbox: package global is nil") {
    auto sb = makeSandbox();
    // package is nil — accessing package.path should error
    CHECK_FALSE(sb->loadScript("return package.path"));
}

TEST_CASE("LuaSandbox: dofile global is nil") {
    auto sb = makeSandbox();
    CHECK_FALSE(sb->loadScript("dofile('anything')"));
}

TEST_CASE("LuaSandbox: loadfile global is nil") {
    auto sb = makeSandbox();
    CHECK_FALSE(sb->loadScript("loadfile('anything')"));
}

// ---------------------------------------------------------------------------
// Custom require loader
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: require outside pack ai directory is rejected") {
    auto sb = makeSandbox(); // empty root — ai/ dir has no files
    bool ok = sb->loadScript("require('socket')");
    CHECK_FALSE(ok);
    // Error should mention directory or module not found
    CHECK_FALSE(sb->lastError().empty());
}

TEST_CASE("LuaSandbox: require for in-pack file that does not exist returns error") {
    // Create a temp dir with no ai/ subdirectory
    auto tmpDir = std::filesystem::temp_directory_path() / "fl_lua_test_nopkg";
    std::filesystem::create_directories(tmpDir);

    auto sb = makeSandbox(tmpDir.string());
    bool ok = sb->loadScript("require('mylib')");
    CHECK_FALSE(ok);
    CHECK_FALSE(sb->lastError().empty());

    std::filesystem::remove_all(tmpDir);
}

// ---------------------------------------------------------------------------
// Allowlisted libraries
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: math library is accessible in sandbox") {
    auto sb = makeSandbox();
    CHECK(sb->loadScript("local x = math.sqrt(4.0)"));
}

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

TEST_CASE("LuaSandbox: syntax error in script sets lastError") {
    auto sb = makeSandbox();
    bool ok = sb->loadScript("this is not valid lua @@@@");
    CHECK_FALSE(ok);
    CHECK_FALSE(sb->lastError().empty());
}

TEST_CASE("LuaSandbox: runtime error in script sets lastError") {
    auto sb = makeSandbox();
    bool ok = sb->loadScript("error('deliberate runtime error')");
    CHECK_FALSE(ok);
    CHECK(sb->lastError().find("deliberate") != std::string::npos);
}
