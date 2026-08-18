// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>

// Forward declaration — callers that dereference the returned pointer must
// include <lua.h> in their own .cpp files. Do NOT wrap it in extern "C" (and do
// not use lua.hpp): Lua is compiled as C++ so that its errors unwind as C++
// exceptions rather than longjmps, which gives its symbols C++ linkage (#1015).
struct lua_State;

namespace fl {

class IFilesystem;

// Where a script's require() reads its modules from (#1210): the pack root together with the
// filesystem that resolves it. The two travel as a pair BECAUSE they were separable before and a
// caller duly supplied a root without one -- `rootDir` is an ASSETS-DOMAIN path ("mods/fl-base-pack",
// what IContentPack::rootDirectory() returns), so reading it with std::ifstream resolved it against
// the process working directory instead and every `require()` failed under --assets / FL_ASSETS_ROOT.
//
//   rootDir: Assets-domain path to the pack root. Empty = this script belongs to no pack (a builtin),
//            and require() is refused.
//   fs:      resolves rootDir in the Assets domain. Null = no filesystem, and require() is refused
//            rather than falling back to a CWD-relative read.
struct ScriptPackSource {
    std::string rootDir;
    IFilesystem* fs{nullptr};
};

// Restricted Lua 5.5 execution environment for AI and mission scripts.
//
// Allowed libraries: math, string, table, coroutine.
// Denied globals:    io, os, package, debug, dofile, loadfile, require
//                    (require is replaced by a custom loader restricted to
//                    the pack's own ai/ subdirectory).
//
// Precompiled Lua bytecode (\x1b magic byte) is rejected by loadScript().
// The Lua state is created on LuaSandbox::create() and destroyed in the
// destructor — lua_close() is called even if the state is never used, which
// satisfies ASAN detect_leaks=1.
class LuaSandbox {
  public:
    ~LuaSandbox();

    // Returns nullptr if the Lua state cannot be allocated.
    // pack: the pack root + the filesystem that resolves it; require() is restricted to
    // <pack.rootDir>/ai/<module>.lua read in the Assets domain (#1210).
    static std::unique_ptr<LuaSandbox> create(ScriptPackSource pack);

    // Compiles and executes Lua source text in the sandbox.
    // Returns false on any error (parse, compile, or runtime).
    // Inspect lastError() for the error message on failure.
    // Rejects source strings that start with \x1b (precompiled bytecode).
    bool loadScript(std::string_view source);

    const std::string& lastError() const;

    // Returns the underlying Lua state for registering additional globals
    // before loadScript() is called. Do not call after loadScript().
    lua_State* luaState() const;

  private:
    LuaSandbox();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
