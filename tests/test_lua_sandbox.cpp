// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaSandbox.h"

#include <stdfs/StdFilesystem.h>

// Lua is compiled as C++, so no extern "C" wrapper (#1015).
#include <lauxlib.h>
#include <lua.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace fl;

// All tests use an empty pack source — the custom require loader will reject all requires (this
// script belongs to no pack), which exercises the error path. Tests that need in-pack require build
// a temporary ASSETS ROOT with a pack directory under it and read through a real StdFilesystem, the
// way fl-server does: `rootDir` is an assets-domain path, never a native one (#1210).

// One temporary assets root holding a pack at `<root>/mods/<packId>`, with the sandbox's pack source
// pointing at the assets-domain path. Removing the directory is the fixture's job so a failing
// REQUIRE cannot leak it.
struct TempPack {
    std::filesystem::path assetsRoot;
    std::string packDir; // assets-domain, e.g. "mods/testpack"
    StdFilesystem fs;

    explicit TempPack(const char* name)
        : assetsRoot(std::filesystem::temp_directory_path() / name), packDir("mods/testpack"),
          fs(std::filesystem::temp_directory_path() / name, std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(assetsRoot);
        std::filesystem::create_directories(assetsRoot / packDir / "ai");
    }
    ~TempPack() {
        std::error_code ec;
        std::filesystem::remove_all(assetsRoot, ec);
    }

    void writeModule(const char* module, std::string_view body) const {
        std::ofstream out(assetsRoot / packDir / "ai" / (std::string(module) + ".lua"), std::ios::binary);
        out << body;
    }
    [[nodiscard]] ScriptPackSource source() {
        return ScriptPackSource{packDir, &fs};
    }
};

static std::unique_ptr<LuaSandbox> makeSandbox(ScriptPackSource pack = {}) {
    auto sb = LuaSandbox::create(std::move(pack));
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
    auto sb = makeSandbox(); // no pack at all — a builtin script has no modules to reach
    bool ok = sb->loadScript("require('socket')");
    CHECK_FALSE(ok);
    // Error should mention directory or module not found
    CHECK_FALSE(sb->lastError().empty());
}

TEST_CASE("LuaSandbox: require for in-pack file that does not exist returns error") {
    TempPack pack("fl_lua_test_nopkg"); // a real pack directory, with no module in it
    auto sb = makeSandbox(pack.source());
    bool ok = sb->loadScript("require('mylib')");
    CHECK_FALSE(ok);
    CHECK(sb->lastError().find("not found") != std::string::npos);
}

// #1210: the pack root is an ASSETS-DOMAIN path, so it means the same thing wherever the process
// happens to be standing. Before the fix the loader handed it to std::ifstream, which resolved it
// against the working directory — every pack script's require() failed under --assets, and the whole
// entity was left with no controller. This runs the successful require from a DIFFERENT directory
// than the assets root, which is the only arrangement that tells the two apart.
TEST_CASE("LuaSandbox: require resolves against the assets root, not the process CWD (#1210)") {
    TempPack pack("fl_lua_test_cwd");
    pack.writeModule("instructor", "return { field_elev = 570 }");

    const std::filesystem::path here = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    auto sb = makeSandbox(pack.source());
    const bool ok = sb->loadScript("local m = require('instructor')\n"
                                   "if m.field_elev ~= 570 then error('wrong module') end");
    const std::string err = sb->lastError();
    std::filesystem::current_path(here); // restore before asserting, so a failure cannot strand the suite

    CHECK(ok);
    CHECK(err.empty());
}

// The other half of the same property: a sandbox with a pack root but NO filesystem refuses rather
// than reading something CWD-relative that happens to be there.
TEST_CASE("LuaSandbox: a pack root with no filesystem refuses require (#1210)") {
    auto sb = makeSandbox(ScriptPackSource{"mods/testpack", nullptr});
    CHECK_FALSE(sb->loadScript("require('anything')"));
    CHECK(sb->lastError().find("no content filesystem") != std::string::npos);
}

// A zero-byte module is an ordinary authoring slip (a file created and not yet written), and it must
// behave like the empty chunk it is: it loads, it returns nothing, and it does not trip the bytecode
// guard — which reads src[0] and would be reading past the end of an empty buffer if the guard were
// written the other way round.
TEST_CASE("LuaSandbox: an empty module file loads and returns nothing (#1210)") {
    TempPack pack("fl_lua_test_emptymod");
    pack.writeModule("blank", "");

    auto sb = makeSandbox(pack.source());
    CHECK(sb->loadScript("local m = require('blank')\n"
                         "if m ~= nil then error('an empty module returned a value') end"));
    CHECK(sb->lastError().empty());
}

// The loader returns every value the chunk produced, not just the first — the shape a module that
// hands back several helpers relies on.
TEST_CASE("LuaSandbox: a module's multiple return values all reach the caller (#1210)") {
    TempPack pack("fl_lua_test_multiret");
    pack.writeModule("pair", "return 7, 35");

    auto sb = makeSandbox(pack.source());
    CHECK(sb->loadScript("local a, b = require('pair')\n"
                         "if a ~= 7 or b ~= 35 then error('wrong values') end"));
    CHECK(sb->lastError().empty());
}

// A BACKSLASH is rejected as well as a forward slash. Windows is a first-class target, so the
// separator a Windows path uses cannot be the one that slips through the traversal guard.
TEST_CASE("LuaSandbox: a backslash in a module name is rejected (#1210)") {
    TempPack pack("fl_lua_test_backslash");
    auto sb = makeSandbox(pack.source());
    CHECK_FALSE(sb->loadScript("require('sub\\\\mod')"));
    CHECK(sb->lastError().find("disallowed") != std::string::npos);
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
        TempPack pack("fl_lua_test_bytecode");
        pack.writeModule("precompiled", "\x1bLua fake bytecode");
        auto sb2 = makeSandbox(pack.source());
        CHECK_FALSE(sb2->loadScript("require('precompiled')"));
        CHECK(sb2->lastError().find("bytecode") != std::string::npos);
    }
    SECTION("syntax error inside the required module") {
        TempPack pack("fl_lua_test_badsyntax");
        pack.writeModule("broken", "this is not @@@ valid lua");
        auto sb2 = makeSandbox(pack.source());
        CHECK_FALSE(sb2->loadScript("require('broken')"));
        CHECK_FALSE(sb2->lastError().empty());
    }
    SECTION("runtime error raised inside the required module") {
        TempPack pack("fl_lua_test_runtimeerr");
        pack.writeModule("thrower", "error('from inside the module')");
        auto sb2 = makeSandbox(pack.source());
        CHECK_FALSE(sb2->loadScript("require('thrower')"));
        CHECK(sb2->lastError().find("from inside the module") != std::string::npos);
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
    TempPack pack("fl_lua_test_goodmod");
    pack.writeModule("mathmod", "return { double = function(x) return x * 2 end }");

    auto sb = makeSandbox(pack.source());
    CHECK(sb->loadScript("local m = require('mathmod')\n"
                         "if m.double(21) ~= 42 then error('wrong result') end"));
    CHECK(sb->lastError().empty());
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
