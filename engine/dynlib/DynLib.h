// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace fl {
class ILogger;
}

// The one dlopen/LoadLibrary in the tree (#163).
//
// It was a file-static helper inside ModLoader.cpp, which was correct while content packs were the
// only thing that could arrive as native code. The AI provider seam is the second, and a second copy
// of "open a shared library, find a factory symbol, report why not" is a second place for the
// Windows/POSIX difference to be got subtly wrong.
//
// Deliberately NOT an RAII handle. Both callers load for process lifetime — a content pack's types
// and a provider's objects outlive any scope that could own the handle, and unloading underneath
// live objects is how you get a crash whose stack frames no longer have symbols. So the handle is
// opened and never closed, which is what the original did, said so, and meant.

namespace fl {

// Load `absolutePath` and resolve `symbol` from it. Returns nullptr and logs the OS's own reason on
// failure — the path was wrong, the library would not load (a missing transitive dependency is the
// common one), or the symbol is absent.
//
// Windows: pass a FULL path. LoadLibrary with a bare name searches, and what it finds is not
// necessarily what the operator installed.
[[nodiscard]] void* loadLibrarySymbol(const std::string& absolutePath, const char* symbol, ILogger& logger);

} // namespace fl
