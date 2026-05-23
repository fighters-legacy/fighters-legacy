include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# Vulkan — system only; no FetchContent fallback (requires LunarG SDK)
# Declared QUIET here; backends that need it call find_package(Vulkan REQUIRED)
# ---------------------------------------------------------------------------
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan: system (${Vulkan_VERSION})")
else()
    message(STATUS "Vulkan: not found — renderer backend will be skipped")
endif()

# ---------------------------------------------------------------------------
# SDL3 — system preferred, FetchContent fallback
# Declared here; platform/vulkan calls FetchContent_MakeAvailable(SDL3) if needed
# ---------------------------------------------------------------------------
find_package(SDL3 3.2 QUIET)
if(SDL3_FOUND)
    message(STATUS "SDL3: system (${SDL3_VERSION})")
else()
    message(STATUS "SDL3: FetchContent (will fetch when renderer backend is configured)")
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.10
        GIT_SHALLOW    TRUE
        SYSTEM
    )
endif()

# ---------------------------------------------------------------------------
# OpenAL Soft — system preferred, FetchContent fallback
# Declared here; platform/openal calls FetchContent_MakeAvailable(openal-soft)
# ---------------------------------------------------------------------------
find_package(OpenAL 1.23 QUIET)
if(OPENAL_FOUND OR OpenAL_FOUND)
    message(STATUS "OpenAL Soft: system")
else()
    message(STATUS "OpenAL Soft: FetchContent (will fetch when audio backend is configured)")
    set(ALSOFT_UTILS    OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openal-soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG        1.23.1
        GIT_SHALLOW    TRUE
        SYSTEM
    )
endif()

# ---------------------------------------------------------------------------
# ENet — FetchContent (no reliable cross-platform system package for 2.x)
# Declared here; platform/net calls FetchContent_MakeAvailable(enet)
# ---------------------------------------------------------------------------
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.17
    GIT_SHALLOW    TRUE
    SYSTEM
)

# ---------------------------------------------------------------------------
# Catch2 — system preferred, FetchContent fallback; always needed for tests
# ---------------------------------------------------------------------------
find_package(Catch2 3 QUIET)
if(Catch2_FOUND)
    message(STATUS "Catch2: system (${Catch2_VERSION})")
else()
    message(STATUS "Catch2: FetchContent")
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)
endif()
