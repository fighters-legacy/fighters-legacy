// SPDX-License-Identifier: GPL-3.0-or-later
#include "script/LuaSandbox.h"

// Lua is compiled as C++ (see cmake/dependencies.cmake), so its symbols have C++
// linkage and these headers must NOT be wrapped in extern "C" — nor may lua.hpp
// be used, since that is precisely an extern "C" wrapper. Building it that way is
// what makes a Lua error a C++ exception rather than a longjmp (#1015).
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

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

// Raising a Lua error from here unwinds to the enclosing lua_pcall as a C++
// exception, because Lua is compiled as C++ (see cmake/dependencies.cmake). Local
// objects are destroyed correctly on the way out, so this reads like ordinary C++
// and needs no special discipline.
//
// This function used to hoist every std::-typed local into an inner scope and
// funnel the outcome through a trivially-destructible enum, on the theory that a
// longjmp would otherwise leak them. That was both insufficient and the source of
// #1015: MSVC's longjmp does not skip C++ frames, it *unwinds* them, and it ran
// these destructors against a stale EH state — faulting in ~ifstream. Do not
// reintroduce that pattern; the fix is the build mode, not the code shape.
static int luaRequireLoader(lua_State* L) {
    const char* root = lua_tostring(L, lua_upvalueindex(1));
    const char* module = luaL_checkstring(L, 1);

    // Reject path traversal and separators: a module name addresses one file
    // inside the pack's own ai/ directory and nothing else.
    for (const char* p = module; *p; ++p) {
        if (*p == '/' || *p == '\\' || (*p == '.' && *(p + 1) == '.'))
            return luaL_error(L, "require: module name '%s' contains disallowed characters", module);
    }

    const std::filesystem::path scriptPath = std::filesystem::path(root) / "ai" / (std::string(module) + ".lua");
    std::ifstream f(scriptPath);
    if (!f.is_open())
        return luaL_error(L, "require: module '%s' not found in pack ai/ directory", module);

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    if (!src.empty() && src[0] == '\x1b')
        return luaL_error(L, "require: precompiled Lua bytecode is not permitted");

    if (luaL_loadbuffer(L, src.c_str(), src.size(), scriptPath.string().c_str()) != LUA_OK)
        return lua_error(L); // error string already on the stack from luaL_loadbuffer

    // Execute the compiled chunk and return all of its values as the module.
    // An error inside the chunk propagates to the enclosing lua_pcall in loadScript().
    lua_call(L, 0, LUA_MULTRET);
    // Stack: [module_name_arg, result0, result1, ...] — return only the results.
    return lua_gettop(L) - 1;
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
