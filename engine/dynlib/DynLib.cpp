// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynlib/DynLib.h"

#include <ILogger.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fl {

void* loadLibrarySymbol(const std::string& absolutePath, const char* symbol, ILogger& logger) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(absolutePath.c_str());
    if (!handle) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   ("failed to load plugin '" + absolutePath + "' via LoadLibrary").c_str());
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    void* sym = reinterpret_cast<void*>(GetProcAddress(handle, symbol));
    if (!sym) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   ("plugin '" + absolutePath + "': symbol '" + symbol + "' not found").c_str());
        FreeLibrary(handle);
        return nullptr;
    }
#else
    void* handle = dlopen(absolutePath.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!handle) {
        const char* err = dlerror();
        logger.log(
            LogLevel::Error, __FILE__, __LINE__,
            (std::string("failed to load plugin '") + absolutePath + "': " + (err ? err : "unknown error")).c_str());
        return nullptr;
    }
    void* sym = dlsym(handle, symbol);
    if (!sym) {
        const char* err = dlerror();
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   (std::string("plugin '") + absolutePath + "': symbol '" + symbol +
                    "' not found: " + (err ? err : "unknown error"))
                       .c_str());
        dlclose(handle);
        return nullptr;
    }
#endif
    // Handle intentionally not stored: plugins load for process lifetime, and unloading one out from
    // under objects it created is a crash with no symbols left to read.
    return sym;
}

} // namespace fl
