// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>

// Restricted Lua 5.4 execution environment for AI and mission scripts.
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
    // packRootDir: absolute or Assets-relative path to the pack root; used
    // to restrict require() to packRootDir/ai/<module>.lua.
    static std::unique_ptr<LuaSandbox> create(std::string packRootDir);

    // Compiles and executes Lua source text in the sandbox.
    // Returns false on any error (parse, compile, or runtime).
    // Inspect lastError() for the error message on failure.
    // Rejects source strings that start with \x1b (precompiled bytecode).
    bool loadScript(std::string_view source);

    const std::string& lastError() const;

  private:
    LuaSandbox();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
