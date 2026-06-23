// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaSandbox.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fl {

// ---------------------------------------------------------------------------
// Pimpl — owns the raw Lua state
// ---------------------------------------------------------------------------

struct LuaSandbox::Impl {
    lua_State* L = nullptr;
    std::string packRootDir;
    std::string lastError;
};

// ---------------------------------------------------------------------------
// Custom require loader — restricted to packRootDir/ai/<module>.lua
// ---------------------------------------------------------------------------

// lua_error() performs a longjmp that bypasses C++ stack unwinding. Any C++
// object with a non-trivial destructor (std::filesystem::path, std::ifstream,
// std::string, std::ostringstream) that is alive when lua_error() fires will
// be leaked, causing LSAN failures. To prevent this, all C++ objects are
// confined to an inner scope that exits *before* any lua_error() call. Status
// is communicated via a plain enum (trivially destructible).
static int luaRequireLoader(lua_State* L) {
    const char* root = lua_tostring(L, lua_upvalueindex(1));
    const char* module = luaL_checkstring(L, 1);

    // Reject path traversal/separators. No C++ objects exist yet — safe to
    // call lua_error() immediately.
    for (const char* p = module; *p; ++p) {
        if (*p == '/' || *p == '\\' || (*p == '.' && *(p + 1) == '.')) {
            return luaL_error(L, "require: module name '%s' contains disallowed characters", module);
        }
    }

    // Trivially-destructible status: no cleanup needed if lua_error longjmps
    // past the switch below.
    enum class Res : uint8_t { Ok, NotFound, Bytecode, LoadErr };
    Res res = Res::NotFound;

    {
        // All C++ objects with non-trivial destructors live in this scope.
        // They are fully destroyed before execution reaches lua_error() below.
        std::filesystem::path scriptPath = std::filesystem::path(root) / "ai" / (std::string(module) + ".lua");
        std::ifstream f(scriptPath);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string src = ss.str();
            if (!src.empty() && src[0] == '\x1b') {
                res = Res::Bytecode;
            } else {
                // Push compiled chunk onto the Lua stack while src is valid.
                // luaL_loadbuffer is a protected call — it cannot longjmp.
                res = (luaL_loadbuffer(L, src.c_str(), src.size(), scriptPath.string().c_str()) == LUA_OK)
                          ? Res::Ok
                          : Res::LoadErr;
            }
        }
    } // scriptPath, f, ss, src all destroyed here

    // All C++ objects are gone. lua_error() / lua_call longjmps are now safe.
    switch (res) {
    case Res::Ok:
        // Execute the compiled chunk and return all its values as the module.
        // lua_call propagates any chunk error to the enclosing lua_pcall (loadScript).
        // At this point no C++ dtors are pending, so longjmp is safe.
        lua_call(L, 0, LUA_MULTRET);
        // Stack: [module_name_arg, result0, result1, ...]; return only the results.
        return lua_gettop(L) - 1;
    case Res::NotFound:
        return luaL_error(L, "require: module '%s' not found in pack ai/ directory", module);
    case Res::Bytecode:
        return luaL_error(L, "require: precompiled Lua bytecode is not permitted");
    case Res::LoadErr:
        return lua_error(L); // error string already on stack from luaL_loadbuffer
    }
    return 0; // unreachable
}

static void installCustomRequire(lua_State* L, const std::string& packRootDir) {
    // Push loader function with packRootDir as upvalue
    lua_pushstring(L, packRootDir.c_str());
    lua_pushcclosure(L, luaRequireLoader, 1);
    lua_setglobal(L, "require");
}

// ---------------------------------------------------------------------------
// LuaSandbox implementation
// ---------------------------------------------------------------------------

LuaSandbox::LuaSandbox() : m_impl(std::make_unique<Impl>()) {}

LuaSandbox::~LuaSandbox() {
    if (m_impl && m_impl->L) {
        lua_close(m_impl->L);
        m_impl->L = nullptr;
    }
}

std::unique_ptr<LuaSandbox> LuaSandbox::create(std::string packRootDir) {
    auto sb = std::unique_ptr<LuaSandbox>(new LuaSandbox());
    sb->m_impl->packRootDir = std::move(packRootDir);

    lua_State* L = luaL_newstate();
    if (!L)
        return nullptr;
    sb->m_impl->L = L;

    // Open only safe standard libraries (base is needed for print, pairs, etc.)
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    lua_pop(L, 1);

    // Nil out dangerous globals from the base library
    static const char* kDenied[] = {"io", "os", "package", "debug", "dofile", "loadfile"};
    for (const char* g : kDenied) {
        lua_pushnil(L);
        lua_setglobal(L, g);
    }

    // Replace require with the pack-scoped loader
    installCustomRequire(L, sb->m_impl->packRootDir);

    return sb;
}

bool LuaSandbox::loadScript(std::string_view source) {
    // Reject precompiled Lua bytecode
    if (!source.empty() && source[0] == '\x1b') {
        m_impl->lastError = "precompiled Lua bytecode is not permitted";
        return false;
    }

    lua_State* L = m_impl->L;

    if (luaL_loadbuffer(L, source.data(), source.size(), "script") != LUA_OK) {
        m_impl->lastError = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        m_impl->lastError = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }

    m_impl->lastError.clear();
    return true;
}

const std::string& LuaSandbox::lastError() const {
    return m_impl->lastError;
}

lua_State* LuaSandbox::luaState() const {
    return m_impl->L;
}

} // namespace fl
