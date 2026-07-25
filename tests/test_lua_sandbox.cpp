// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaSandbox.h"

// Lua is compiled as C++, so no extern "C" wrapper (#1015).
#include <lauxlib.h>
#include <lua.h>

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

// #1015: the require loader raises its "not found" error from a C++ frame that
// owns a std::filesystem::path, a std::ifstream and a std::ostringstream. Built
// as C, Lua raises by longjmp — which MSVC implements by *unwinding* the frames
// it passes, running those destructors against an EH state that no longer
// describes live objects, faulting inside ~ifstream. Building Lua as C++ turns
// the raise into a C++ exception that unwinds correctly.
//
// These cases hammer that path rather than touching it once: a single failed
// require can get lucky with stack layout (the original bug reproduced only in
// some builds of the very same source), so repetition is the point.
TEST_CASE("LuaSandbox: repeated failing requires unwind cleanly (#1015)") {
    auto sb = makeSandbox();
    for (int i = 0; i < 200; ++i) {
        CHECK_FALSE(sb->loadScript("require('no_such_module')"));
        CHECK_FALSE(sb->lastError().empty());
    }
}

TEST_CASE("LuaSandbox: every require rejection path unwinds cleanly (#1015)") {
    // Each reaches a different raise site in the loader, and all of them are
    // reached with C++ objects owned by the enclosing frame.
    auto sb = makeSandbox();

    SECTION("module not found") {
        CHECK_FALSE(sb->loadScript("require('absent')"));
        CHECK(sb->lastError().find("not found") != std::string::npos);
    }
    SECTION("path separator rejected") {
        CHECK_FALSE(sb->loadScript("require('sub/mod')"));
        CHECK(sb->lastError().find("disallowed") != std::string::npos);
    }
    SECTION("parent traversal rejected") {
        CHECK_FALSE(sb->loadScript("require('..evil')"));
        CHECK(sb->lastError().find("disallowed") != std::string::npos);
    }
    SECTION("bytecode rejected") {
        auto tmpDir = std::filesystem::temp_directory_path() / "fl_lua_test_bytecode";
        std::filesystem::create_directories(tmpDir / "ai");
        {
            std::ofstream out(tmpDir / "ai" / "precompiled.lua", std::ios::binary);
            out << "\x1bLua fake bytecode";
        }
        auto sb2 = makeSandbox(tmpDir.string());
        CHECK_FALSE(sb2->loadScript("require('precompiled')"));
        CHECK(sb2->lastError().find("bytecode") != std::string::npos);
        std::filesystem::remove_all(tmpDir);
    }
    SECTION("syntax error inside the required module") {
        auto tmpDir = std::filesystem::temp_directory_path() / "fl_lua_test_badsyntax";
        std::filesystem::create_directories(tmpDir / "ai");
        {
            std::ofstream out(tmpDir / "ai" / "broken.lua");
            out << "this is not @@@ valid lua";
        }
        auto sb2 = makeSandbox(tmpDir.string());
        CHECK_FALSE(sb2->loadScript("require('broken')"));
        CHECK_FALSE(sb2->lastError().empty());
        std::filesystem::remove_all(tmpDir);
    }
    SECTION("runtime error raised inside the required module") {
        auto tmpDir = std::filesystem::temp_directory_path() / "fl_lua_test_runtimeerr";
        std::filesystem::create_directories(tmpDir / "ai");
        {
            std::ofstream out(tmpDir / "ai" / "thrower.lua");
            out << "error('from inside the module')";
        }
        auto sb2 = makeSandbox(tmpDir.string());
        CHECK_FALSE(sb2->loadScript("require('thrower')"));
        CHECK(sb2->lastError().find("from inside the module") != std::string::npos);
        std::filesystem::remove_all(tmpDir);
    }
}

// ---------------------------------------------------------------------------
// The invariant the fix rests on
// ---------------------------------------------------------------------------

// This is the guard rail for #1015. Everything above tests the *symptom* — the
// require paths that happened to crash. This tests the *property*: that a Lua
// error unwinds C++ frames and runs their destructors exactly once.
//
// If Lua is ever built as C again (a reintroduced system-Lua path, a toolchain
// that ignores LANGUAGE CXX), errors go back to longjmp and this fails loudly:
// on the Itanium ABI the destructor is skipped and `destroyed` stays false; on
// MSVC the unwind runs it against a dead frame and the process faults. Either
// way the build mode is caught here rather than in a crash months later.
namespace {

bool g_probeDestroyed = false;

struct DestructorProbe {
    ~DestructorProbe() {
        g_probeDestroyed = true;
    }
};

int raiseWithLiveCxxObject(lua_State* L) {
    DestructorProbe probe; // deliberately alive across the raise
    (void)probe;
    return luaL_error(L, "raised while a C++ object was alive");
}

} // namespace

TEST_CASE("LuaSandbox: a Lua error unwinds C++ frames and runs destructors (#1015)") {
    auto sb = makeSandbox();
    lua_State* L = sb->luaState();
    REQUIRE(L != nullptr);

    g_probeDestroyed = false;
    lua_pushcfunction(L, raiseWithLiveCxxObject);
    lua_setglobal(L, "raise_probe");

    CHECK_FALSE(sb->loadScript("raise_probe()"));
    CHECK(sb->lastError().find("raised while a C++ object was alive") != std::string::npos);
    CHECK(g_probeDestroyed);
}

TEST_CASE("LuaSandbox: a successful require still works (#1015)") {
    auto tmpDir = std::filesystem::temp_directory_path() / "fl_lua_test_goodmod";
    std::filesystem::create_directories(tmpDir / "ai");
    {
        std::ofstream out(tmpDir / "ai" / "mathmod.lua");
        out << "return { double = function(x) return x * 2 end }";
    }

    auto sb = makeSandbox(tmpDir.string());
    CHECK(sb->loadScript("local m = require('mathmod')\n"
                         "if m.double(21) ~= 42 then error('wrong result') end"));
    CHECK(sb->lastError().empty());

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
