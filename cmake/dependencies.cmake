include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# CMake 3.30+ deprecates FetchContent_Populate(name) for deps without CMakeLists.txt
# (stb, lua_src). Set CMP0169=OLD to suppress the warning; we have no alternative path.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

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
find_package(SDL3 3.4.10 QUIET)
if(SDL3_FOUND)
    message(STATUS "SDL3: system (${SDL3_VERSION})")
else()
    message(STATUS "SDL3: FetchContent (will fetch when renderer backend is configured)")
    # Force static library so release binaries are self-contained (no SDL3.dll / libSDL3.so).
    # These are SDL3-specific cache vars; they don't affect any other FetchContent dep.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    # Headless builds (renderer disabled, e.g. the fl-server + bot_swarm load harness or the
    # scale-gate CI leg) never link platform-sdl3, but SDL3's CMakeLists is still processed by the
    # unconditional platform/sdl3 subdir and hard-errors (FATAL_ERROR) when it can find neither X11
    # nor Wayland dev libraries. Since nothing compiles SDL3 in a no-Vulkan configure, let it build
    # as a console-only SDL so the configure needs zero X11/Wayland/desktop dev packages (#718).
    # Client builds have Vulkan and keep the full windowing SDL unchanged.
    if(NOT Vulkan_FOUND)
        set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.10
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
endif()

# ---------------------------------------------------------------------------
# OpenAL Soft — system preferred, FetchContent fallback
# Declared here; platform/openal calls FetchContent_MakeAvailable(openal-soft)
# Skip find_package on Apple: the deprecated system OpenAL.framework passes the
# 1.24.2 version check but provides <OpenAL/al.h> as <OpenAL/al.h> (framework
# layout), not <AL/al.h>. Always fetch OpenAL Soft on Apple.
# ---------------------------------------------------------------------------
if(NOT APPLE)
    find_package(OpenAL 1.24.2 QUIET)
endif()
if(OPENAL_FOUND OR OpenAL_FOUND)
    message(STATUS "OpenAL Soft: system")
else()
    message(STATUS "OpenAL Soft: FetchContent (will fetch when audio backend is configured)")
    set(ALSOFT_UTILS           OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES        OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BUILD_IMPORT_LIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openal-soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG        1.24.2
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
endif()

# ---------------------------------------------------------------------------
# enet6 — FetchContent (no reliable cross-platform system package)
# IPv4+IPv6 dual-stack fork of ENet; MIT licensed. Declared here;
# platform/net calls FetchContent_MakeAvailable(enet6).
# ---------------------------------------------------------------------------
FetchContent_Declare(enet6
    GIT_REPOSITORY https://github.com/SirLynix/enet6.git
    GIT_TAG        v6.1.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
)

# ---------------------------------------------------------------------------
# GameNetworkingSockets (#507) — the 128+ transport behind INetwork.
# Declared here (gated on FL_ENABLE_GNS); platform/net links OpenSSL + protobuf and calls
# FetchContent_MakeAvailable(GameNetworkingSockets).
#
# Crypto = OpenSSL (GNS rejects libsodium AES on ARM / Apple Silicon, see docs/transport-selection.md).
# protobuf = SYSTEM. find_package(Protobuf) here both gates GNS on availability AND seeds the module
# cache vars that GNS's own find_package(Protobuf) reuses. If OpenSSL or system protobuf is absent,
# FL_ENABLE_GNS is force-disabled and the build stays enet6-only — so build legs that don't install
# the deps configure fine. GNS v1.6.0 needs the PRE-ABSEIL protobuf 3.21.x line; CI sources it per
# platform (#653): Linux = apt libprotobuf-dev, macOS = pinned Homebrew protobuf@21 via
# CMAKE_PREFIX_PATH (avoids the abseil-based `protobuf` 5.x formula, whose CMake config clashes in a
# mixed module/config double-find — "some but not all targets already defined"/libupb), Windows =
# the repo-root vcpkg.json manifest pinning protobuf 3.21.12 under the vcpkg toolchain. The CI legs
# assert FL_ENABLE_GNS stayed ON post-configure, so this graceful fallback can't mask a broken leg.
# No WebRTC/ICE (dedicated-server) ⇒ no abseil.
# ---------------------------------------------------------------------------
if(FL_ENABLE_GNS)
    # Prefer the STATIC protobuf archive. FindProtobuf reads this at FIND time (it toggles
    # CMAKE_FIND_LIBRARY_SUFFIXES to prefer libprotobuf.a), so it MUST be set before the seeding
    # find_package below — GNS's own later find_package(Protobuf) short-circuits to the cached
    # Protobuf_LIBRARY, so if this seed caches the shared .so the whole chain links it. Getting this
    # wrong shipped a release fl-server dynamically linked against libprotobuf.so.32, which will not
    # load on a machine without that exact private build (#905).
    set(Protobuf_USE_STATIC_LIBS ON)
    find_package(OpenSSL 1.1.1 QUIET) # 1.1.1+ for GNS's EVP_PKEY 25519 raw-key API
    find_package(Protobuf QUIET)      # module mode; seeds Protobuf_* cache for GNS's find_package (static)
    if(OpenSSL_FOUND AND Protobuf_FOUND)
        message(STATUS "GameNetworkingSockets: enabled (OpenSSL ${OPENSSL_VERSION}, "
                       "system protobuf ${Protobuf_VERSION})")
    else()
        message(WARNING "FL_ENABLE_GNS=ON but OpenSSL>=1.1.1 + system protobuf were not both found — "
                        "building enet6-only. Install libssl-dev + libprotobuf-dev + protobuf-compiler "
                        "(Linux) to enable GNS.")
        set(FL_ENABLE_GNS OFF CACHE BOOL "" FORCE)
    endif()
endif()

if(FL_ENABLE_GNS)
    # -- GameNetworkingSockets v1.6.0: static only, no P2P/ICE/tests, OpenSSL crypto --
    set(BUILD_STATIC_LIB    ON  CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB    OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS         OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS         OFF CACHE BOOL "" FORCE)
    set(ENABLE_ICE          OFF CACHE BOOL "" FORCE) # dedicated-server: no NAT-punch P2P
    set(USE_STEAMWEBRTC     OFF CACHE BOOL "" FORCE) # ⇒ no webrtc/abseil submodules
    set(USE_CRYPTO          "OpenSSL" CACHE STRING "" FORCE)
    set(USE_CRYPTO25519     "OpenSSL" CACHE STRING "" FORCE)
    # Protobuf_USE_STATIC_LIBS was set (as a normal var, before the seeding find_package) in the block
    # above — setting it here would be too late to affect the already-cached Protobuf_LIBRARY (#905).
    FetchContent_Declare(GameNetworkingSockets
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG        v1.6.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        GIT_SUBMODULES "" # ICE/WebRTC off ⇒ skip abseil/webrtc/vjson submodules (webrtc is huge)
        SYSTEM
    )
endif()

# ---------------------------------------------------------------------------
# Catch2 — system preferred, FetchContent fallback; always needed for tests
# ---------------------------------------------------------------------------
find_package(Catch2 3.15.0 QUIET)
if(Catch2_FOUND)
    message(STATUS "Catch2: system (${Catch2_VERSION})")
else()
    message(STATUS "Catch2: FetchContent")
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.15.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# ---------------------------------------------------------------------------
# tinygltf — header-only glTF 2.0 loader; system preferred, FetchContent fallback
# Used by tools/validate-mesh, platform-vulkan/platform-meshgraph (mesh upload, #839) and
# engine-render (the articulation rig, #840) — all through the shared tinygltf-impl TU below.
# ---------------------------------------------------------------------------
find_package(tinygltf 3.0.0 QUIET)
if(tinygltf_FOUND)
    message(STATUS "tinygltf: system (${tinygltf_VERSION})")
else()
    message(STATUS "tinygltf: FetchContent")
    # Force header-only mode: prevents tinygltf from compiling tiny_gltf.cc as
    # a library target, which would inherit the project's -Werror flags and fail
    # on -Wmissing-field-initializers in stb_image_write.h.
    set(TINYGLTF_HEADER_ONLY          ON  CACHE BOOL "" FORCE)
    set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(TINYGLTF_BUILD_GL_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(TINYGLTF_BUILD_VALIDATOR      OFF CACHE BOOL "" FORCE)
    set(TINYGLTF_BUILD_BUILDER        OFF CACHE BOOL "" FORCE)
    set(TINYGLTF_INSTALL              OFF CACHE BOOL "" FORCE)
    # tinygltf uses cmake_minimum_required(VERSION 3.6); CMake 4.x warns on anything
    # below 3.10. Use CMAKE_POLICY_VERSION_MINIMUM to silence it.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        set(CMAKE_POLICY_VERSION_MINIMUM "3.10" CACHE INTERNAL "")
    endif()
    FetchContent_Declare(tinygltf
        GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
        GIT_TAG        v3.0.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(tinygltf)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
    endif()
endif()

# The single tinygltf + stb implementation TU for the whole build (#836). Both platform-vulkan and
# validate-mesh-lib link this instead of each emitting their own implementation, so a binary that
# pulls both (fl-viewer) gets exactly one copy and does not clash at link time. -w on the one vendored
# TU (stb/tinygltf trip -Werror on -Wmissing-field-initializers etc.).
add_library(tinygltf-impl STATIC ${CMAKE_SOURCE_DIR}/third_party/tinygltf_impl.cpp)
target_include_directories(tinygltf-impl SYSTEM PUBLIC ${tinygltf_SOURCE_DIR})
set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/tinygltf_impl.cpp PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

# ---------------------------------------------------------------------------
# yaml-cpp — YAML parser; system preferred, FetchContent fallback
# Used only by tools/validate-mission.
# ---------------------------------------------------------------------------
find_package(yaml-cpp 0.9.0 QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "yaml-cpp: system (${yaml-cpp_VERSION})")
else()
    message(STATUS "yaml-cpp: FetchContent")
    set(YAML_CPP_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB     OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # yaml-cpp 0.9.0 may still declare a cmake_minimum_required below 3.5; CMake 4.x
    # rejects these. CMAKE_POLICY_VERSION_MINIMUM is the CMake 4.x mechanism for this.
    # Keep the workaround until yaml-cpp bumps their minimum to 3.5+.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE INTERNAL "")
    endif()
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG        yaml-cpp-0.9.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(yaml-cpp)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
    endif()
    # yaml-cpp 0.9.0 adds fptostring.cpp / dragonbox.h with size_t→int narrowing that
    # MSVC (C4267) and GCC 14+ (missing <cstdint>) reject under -Werror / /WX.
    if(TARGET yaml-cpp)
        target_compile_options(yaml-cpp PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w -include cstdint>
            $<$<CXX_COMPILER_ID:MSVC>:/W0 /FI cstdint>
        )
        set_target_properties(yaml-cpp PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator — header-only; gated on Vulkan_FOUND (Vulkan backend only)
# ---------------------------------------------------------------------------
if(Vulkan_FOUND)
    find_package(VulkanMemoryAllocator 3.4.0 QUIET)
    if(VulkanMemoryAllocator_FOUND)
        message(STATUS "VulkanMemoryAllocator: system")
    else()
        message(STATUS "VulkanMemoryAllocator: FetchContent")
        FetchContent_Declare(VulkanMemoryAllocator
            GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
            GIT_TAG        v3.4.0
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
            SYSTEM
        )
        FetchContent_MakeAvailable(VulkanMemoryAllocator)
    endif()

    # ---------------------------------------------------------------------------
    # KTX-Software — KTX2 + Basis Universal transcode; gated on Vulkan_FOUND
    # Read-only static library (ktx_read); write/encode features disabled.
    # ---------------------------------------------------------------------------
    find_package(ktx 4.4.2 QUIET)
    if(ktx_FOUND)
        message(STATUS "KTX-Software: system")
    else()
        message(STATUS "KTX-Software: FetchContent")
        set(KTX_FEATURE_TESTS        OFF CACHE BOOL "" FORCE)
        set(KTX_FEATURE_TOOLS        OFF CACHE BOOL "" FORCE)
        set(KTX_FEATURE_GL_UPLOAD    OFF CACHE BOOL "" FORCE)
        set(KTX_FEATURE_VK_UPLOAD    OFF CACHE BOOL "" FORCE)
        set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
        set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
        set(KTX_FEATURE_TOOLS_CTS    OFF CACHE BOOL "" FORCE)
        set(BASISU_SUPPORT_OPENCL    OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(ktx
            GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
            GIT_TAG        v4.4.2
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
            SYSTEM
        )
        # KTX-Software has many pedantic issues in its internals (anonymous structs,
        # etc.).  Disable warning-as-error for the entire KTX subdirectory so all
        # object library targets build cleanly; then also add -w to the final
        # library targets to silence warning noise in the build output.
        set(_fl_save_werror "${CMAKE_COMPILE_WARNING_AS_ERROR}")
        # KTX_FEATURE_STATIC_LIBRARY=ON (above) controls supplemental static targets;
        # BUILD_SHARED_LIBS is what actually sets LIB_TYPE in KTX's CMakeLists.
        set(_fl_ktx_save_bsl "${BUILD_SHARED_LIBS}")
        set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(ktx)
        set(CMAKE_COMPILE_WARNING_AS_ERROR "${_fl_save_werror}")
        unset(BUILD_SHARED_LIBS CACHE)
        if(_fl_ktx_save_bsl)
            set(BUILD_SHARED_LIBS "${_fl_ktx_save_bsl}")
        endif()
        # Suppress warnings from the compiled ktx_read target (it inherits -Werror
        # from the outer project otherwise).
        foreach(_ktx_target IN ITEMS ktx_read ktx)
            if(TARGET ${_ktx_target})
                get_target_property(_type ${_ktx_target} TYPE)
                if(_type MATCHES "LIBRARY")
                    target_compile_options(${_ktx_target} PRIVATE
                        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
                        $<$<CXX_COMPILER_ID:MSVC>:/W0>
                    )
                endif()
            endif()
        endforeach()
    endif()

    # ---------------------------------------------------------------------------
    # Dear ImGui — immediate-mode GUI behind the IGui HAL (#156; platform-gui, Vulkan-gated).
    # Ships loose sources + backends/ with no CMakeLists, so populate-only (the stb/Lua idiom);
    # the actual `imgui` static library target is built in platform/gui/CMakeLists.txt from
    # ${imgui_SOURCE_DIR}. MIT-licensed (recorded in docs/development.md).
    # ---------------------------------------------------------------------------
    message(STATUS "Dear ImGui: FetchContent (populate-only)")
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()
endif()

# ---------------------------------------------------------------------------
# GLM — header-only math library; unconditional (shared by platform-hal and renderer)
# ---------------------------------------------------------------------------
find_package(glm 1.0.3 QUIET)
if(glm_FOUND)
    message(STATUS "glm: system (${glm_VERSION})")
else()
    message(STATUS "glm: FetchContent")
    # GLM 1.0.1 defaults GLM_BUILD_LIBRARY ON, which builds glm.dll on Windows.
    # The DLL has no exports so no glm.lib is generated, breaking all link steps.
    # Force header-only mode — we only use GLM as headers.
    set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.3
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(glm)
endif()

# ---------------------------------------------------------------------------
# zstd — snapshot payload compression at the engine layer (#775; engine-compress).
# System-preferred. Three tiers, normalized onto one imported/interface target
# `fl::zstd`, because distros disagree about what a zstd dev package provides:
#  1. find_package(zstd CONFIG) — upstream's own CMake config (Fedora, brew, vcpkg).
#  2. find_path/find_library — Debian/Ubuntu libzstd-dev ships only headers +
#     pkg-config, no CMake config.
#  3. FetchContent v1.5.7 (build/cmake subdir; static, programs/tests off) —
#     Windows CI and any box with no dev package. BSD-3.
# ---------------------------------------------------------------------------
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static OR TARGET zstd::libzstd)
    add_library(fl-zstd INTERFACE)
    if(TARGET zstd::libzstd_shared)
        target_link_libraries(fl-zstd INTERFACE zstd::libzstd_shared)
    elseif(TARGET zstd::libzstd_static)
        target_link_libraries(fl-zstd INTERFACE zstd::libzstd_static)
    else()
        target_link_libraries(fl-zstd INTERFACE zstd::libzstd)
    endif()
    add_library(fl::zstd ALIAS fl-zstd)
    message(STATUS "zstd: system (CMake config)")
else()
    find_path(FL_ZSTD_INCLUDE_DIR zstd.h)
    find_library(FL_ZSTD_LIBRARY NAMES zstd libzstd)
    if(FL_ZSTD_INCLUDE_DIR AND FL_ZSTD_LIBRARY)
        add_library(fl-zstd INTERFACE)
        target_include_directories(fl-zstd SYSTEM INTERFACE ${FL_ZSTD_INCLUDE_DIR})
        target_link_libraries(fl-zstd INTERFACE ${FL_ZSTD_LIBRARY})
        add_library(fl::zstd ALIAS fl-zstd)
        message(STATUS "zstd: system (${FL_ZSTD_LIBRARY})")
    else()
        message(STATUS "zstd: FetchContent")
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(zstd_src
            GIT_REPOSITORY https://github.com/facebook/zstd.git
            GIT_TAG        v1.5.7
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
            SOURCE_SUBDIR  build/cmake
            SYSTEM
        )
        FetchContent_MakeAvailable(zstd_src)
        # Third-party C sources must not inherit -Werror (see the Lua block above).
        set_target_properties(libzstd_static PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
        add_library(fl-zstd INTERFACE)
        target_link_libraries(fl-zstd INTERFACE libzstd_static)
        target_include_directories(fl-zstd SYSTEM INTERFACE "${zstd_src_SOURCE_DIR}/lib")
        add_library(fl::zstd ALIAS fl-zstd)
    endif()
endif()

# ---------------------------------------------------------------------------
# stb — single-file C libraries; used for stb_image 16-bit PNG decode in
# engine-render (TerrainChunkIO). stb has no CMakeLists.txt; use
# FetchContent_Populate to download source only.
# (stb_vorbis is NOT used: it is a trusted-input decoder and was replaced by
# libogg/libvorbis for the attacker-controlled content-pack audio path, #723.)
# ---------------------------------------------------------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW    FALSE
    GIT_PROGRESS   TRUE
    SYSTEM
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# ---------------------------------------------------------------------------
# libogg + libvorbis (vorbisfile) — reference OGG Vorbis decoder for engine-audio (#723).
# FetchContent-always (pinned, static), NOT system-preferred: content-pack audio is
# attacker-controlled and this decoder is a fuzz target (fuzz_ogg), so the exact
# version must be deterministic across all platforms and CI legs. Both are BSD-3.
# OVERRIDE_FIND_PACKAGE redirects libvorbis's internal find_package(Ogg REQUIRED)
# to this build (the redirects dir outranks vorbis's bundled FindOgg.cmake since
# CMake 3.24; if that ever regresses, set OGG_INCLUDE_DIR/OGG_LIBRARY instead).
# ---------------------------------------------------------------------------
FetchContent_Declare(Ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG        v1.3.6
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
    OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(Vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG        v1.3.7
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
)
# Shadow BUILD_TESTING with a plain variable for the subproject scope only — libogg
# would otherwise register its test_bitwise/test_framing ctest entries in our suite.
# A plain set() shadows the cache variable for the add_subdirectory scopes below and
# is restored afterwards, so the project's own test registration is unaffected.
set(FL_SAVED_BUILD_TESTING "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(INSTALL_DOCS OFF)
FetchContent_MakeAvailable(Ogg)
if(NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
endif()
# vorbis v1.3.7 declares a cmake_minimum_required below 3.5, which CMake 4.x rejects.
# CMAKE_POLICY_VERSION_MINIMUM is the CMake 4.x mechanism for this (same workaround
# as tinygltf/yaml-cpp); keep until a vorbis release bumps its minimum to 3.5+.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE INTERNAL "")
endif()
FetchContent_MakeAvailable(Vorbis)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
endif()
set(BUILD_TESTING "${FL_SAVED_BUILD_TESTING}")
unset(FL_SAVED_BUILD_TESTING)
unset(INSTALL_DOCS)
# Prevent the vendored C sources from inheriting CMAKE_COMPILE_WARNING_AS_ERROR=ON
# (same pattern as the Lua FetchContent fallback).
set_target_properties(ogg vorbis vorbisenc vorbisfile PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# ---------------------------------------------------------------------------
# Lua 5.5 — system preferred, FetchContent fallback
# Used by engine/script/LuaSandbox for sandboxed AI and mission script execution.
# FindLua provides variables, not targets; create a uniform lua::lua INTERFACE
# target in both paths so downstream CMakeLists can always link lua::lua.
# ---------------------------------------------------------------------------
find_package(Lua 5.5.0 QUIET)
if(LUA_FOUND)
    message(STATUS "Lua: system (${LUA_VERSION_STRING})")
    add_library(lua_system_iface INTERFACE)
    target_include_directories(lua_system_iface INTERFACE ${LUA_INCLUDE_DIR})
    target_link_libraries(lua_system_iface INTERFACE ${LUA_LIBRARIES})
    add_library(lua::lua ALIAS lua_system_iface)
else()
    message(STATUS "Lua: FetchContent (lua/lua v5.5.0)")
    FetchContent_Declare(lua_src
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG        v5.5.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_GetProperties(lua_src)
    if(NOT lua_src_POPULATED)
        FetchContent_Populate(lua_src)
    endif()
    file(GLOB LUA_C_SOURCES "${lua_src_SOURCE_DIR}/*.c")
    # Exclude the standalone interpreter (lua.c), compiler (luac.c), and the
    # single-file amalgamation (onelua.c) which #includes every other .c file —
    # including it alongside the individual sources causes duplicate symbol
    # definitions that produce LNK4006 warnings and crash MSVC test binaries.
    list(FILTER LUA_C_SOURCES EXCLUDE REGEX ".*/lua\\.c$")
    list(FILTER LUA_C_SOURCES EXCLUDE REGEX ".*/luac\\.c$")
    list(FILTER LUA_C_SOURCES EXCLUDE REGEX ".*/onelua\\.c$")
    add_library(lua54_static STATIC ${LUA_C_SOURCES})
    set_target_properties(lua54_static PROPERTIES C_STANDARD 99)
    # Prevent Lua's own C sources from inheriting CMAKE_COMPILE_WARNING_AS_ERROR=ON.
    set_target_properties(lua54_static PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    target_compile_options(lua54_static PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
        $<$<CXX_COMPILER_ID:MSVC>:/W0>
    )
    target_include_directories(lua54_static PUBLIC ${lua_src_SOURCE_DIR})
    add_library(lua::lua ALIAS lua54_static)
endif()

# ---------------------------------------------------------------------------
# tomlplusplus — header-only TOML parser; system preferred, FetchContent fallback
# Used by engine/content/ModLoader to parse mod manifests.
# ---------------------------------------------------------------------------
find_package(tomlplusplus 3.4.0 QUIET)
if(tomlplusplus_FOUND)
    message(STATUS "tomlplusplus: system (${tomlplusplus_VERSION})")
else()
    message(STATUS "tomlplusplus: FetchContent")
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# ---------------------------------------------------------------------------
# Opus — the voice codec for the in-game radio nets (Epic J, #531/#532).
# System-preferred with a FetchContent fallback, normalized onto `fl::opus`
# (the zstd pattern), because distros disagree about what an opus dev package
# provides: Fedora/brew/vcpkg ship upstream's OpusConfig.cmake, Debian/Ubuntu
# ship only headers + pkg-config.
#
# Unconditional (not gated behind an option) on purpose: a build flag here would
# create a second, differently-shaped voice path that CI would never exercise
# evenly. libopus is ~1 MB of dependency-free C with a native CMake build; the
# cost of always having it is far below the cost of two code paths. BSD-3.
# Note the codec is only ever fed OUR OWN encoder's output relayed by the
# server — but the server never decodes, so a client decodes attacker-supplied
# bytes; payload length is capped on both sides (see kMaxVoicePayloadBytes).
# ---------------------------------------------------------------------------
find_package(Opus CONFIG QUIET)
if(TARGET Opus::opus)
    add_library(fl-opus INTERFACE)
    target_link_libraries(fl-opus INTERFACE Opus::opus)
    add_library(fl::opus ALIAS fl-opus)
    message(STATUS "opus: system (CMake config)")
else()
    find_path(FL_OPUS_INCLUDE_DIR opus/opus.h)
    find_library(FL_OPUS_LIBRARY NAMES opus libopus)
    if(FL_OPUS_INCLUDE_DIR AND FL_OPUS_LIBRARY)
        add_library(fl-opus INTERFACE)
        # Upstream headers include each other as "opus_types.h", so the opus/ subdir
        # must be on the include path too, not just its parent.
        target_include_directories(fl-opus SYSTEM INTERFACE
            ${FL_OPUS_INCLUDE_DIR} ${FL_OPUS_INCLUDE_DIR}/opus)
        target_link_libraries(fl-opus INTERFACE ${FL_OPUS_LIBRARY})
        add_library(fl::opus ALIAS fl-opus)
        message(STATUS "opus: system (${FL_OPUS_LIBRARY})")
    else()
        message(STATUS "opus: FetchContent")
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(opus_src
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        v1.5.2
            GIT_SHALLOW    TRUE
            GIT_PROGRESS   TRUE
            SYSTEM
        )
        FetchContent_MakeAvailable(opus_src)
        # Third-party C sources must not inherit -Werror (see the Lua/zstd blocks above).
        set_target_properties(opus PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
        add_library(fl-opus INTERFACE)
        target_link_libraries(fl-opus INTERFACE opus)
        add_library(fl::opus ALIAS fl-opus)
    endif()
endif()
