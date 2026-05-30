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
# Skip find_package on Apple: the deprecated system OpenAL.framework passes the
# 1.23 version check but provides <OpenAL/al.h> as <OpenAL/al.h> (framework
# layout), not <AL/al.h>. Always fetch OpenAL Soft on Apple.
# ---------------------------------------------------------------------------
if(NOT APPLE)
    find_package(OpenAL 1.23 QUIET)
endif()
if(OPENAL_FOUND OR OpenAL_FOUND)
    message(STATUS "OpenAL Soft: system")
else()
    message(STATUS "OpenAL Soft: FetchContent (will fetch when audio backend is configured)")
    set(ALSOFT_UTILS    OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openal-soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG        1.24.2
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

# ---------------------------------------------------------------------------
# tinygltf — header-only glTF 2.0 loader; system preferred, FetchContent fallback
# Used only by tools/validate-mesh.
# ---------------------------------------------------------------------------
find_package(tinygltf QUIET)
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
    FetchContent_Declare(tinygltf
        GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
        GIT_TAG        v2.9.3
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(tinygltf)
endif()

# ---------------------------------------------------------------------------
# yaml-cpp — YAML parser; system preferred, FetchContent fallback
# Used only by tools/validate-mission.
# ---------------------------------------------------------------------------
find_package(yaml-cpp 0.8 QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "yaml-cpp: system (${yaml-cpp_VERSION})")
else()
    message(STATUS "yaml-cpp: FetchContent")
    set(YAML_CPP_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB     OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # yaml-cpp 0.8.0 declares cmake_minimum_required(VERSION 2.8.12); CMake 4.x
    # rejects minimum versions below 3.5. CMAKE_POLICY_VERSION_MINIMUM is the
    # CMake 4.x mechanism for this (advertised in the cmake_minimum_required error
    # message itself). Set it as a cache variable so it is visible when CMake
    # configures the FetchContent subdirectory.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE INTERNAL "")
    endif()
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG        0.8.0
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(yaml-cpp)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
    endif()
    # yaml-cpp 0.8.0 uses uint16_t in emitterutils.cpp without including <cstdint>.
    # Force-include it so GCC 14+ doesn't reject the missing declaration as an error.
    if(TARGET yaml-cpp)
        target_compile_options(yaml-cpp PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-include cstdint>
            $<$<CXX_COMPILER_ID:MSVC>:/FI cstdint>
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator — header-only; gated on Vulkan_FOUND (Vulkan backend only)
# ---------------------------------------------------------------------------
if(Vulkan_FOUND)
    find_package(VulkanMemoryAllocator QUIET)
    if(VulkanMemoryAllocator_FOUND)
        message(STATUS "VulkanMemoryAllocator: system")
    else()
        message(STATUS "VulkanMemoryAllocator: FetchContent")
        FetchContent_Declare(VulkanMemoryAllocator
            GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
            GIT_TAG        v3.3.0
            GIT_SHALLOW    TRUE
            SYSTEM
        )
        FetchContent_MakeAvailable(VulkanMemoryAllocator)
    endif()

    # ---------------------------------------------------------------------------
    # KTX-Software — KTX2 + Basis Universal transcode; gated on Vulkan_FOUND
    # Read-only static library (ktx_read); write/encode features disabled.
    # ---------------------------------------------------------------------------
    find_package(ktx QUIET)
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
            SYSTEM
        )
        # KTX-Software has many pedantic issues in its internals (anonymous structs,
        # etc.).  Disable warning-as-error for the entire KTX subdirectory so all
        # object library targets build cleanly; then also add -w to the final
        # library targets to silence warning noise in the build output.
        set(_fl_save_werror "${CMAKE_COMPILE_WARNING_AS_ERROR}")
        set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
        FetchContent_MakeAvailable(ktx)
        set(CMAKE_COMPILE_WARNING_AS_ERROR "${_fl_save_werror}")
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
endif()

# ---------------------------------------------------------------------------
# GLM — header-only math library; unconditional (shared by platform-hal and renderer)
# ---------------------------------------------------------------------------
find_package(glm QUIET)
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
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(glm)
endif()

# ---------------------------------------------------------------------------
# stb — single-file C libraries; used for stb_vorbis OGG decode in engine-audio.
# stb has no CMakeLists.txt; use FetchContent_Populate to download source only.
# stb_vorbis.c is compiled directly as a source file in the engine-audio target.
# ---------------------------------------------------------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW    FALSE
    SYSTEM
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# ---------------------------------------------------------------------------
# tomlplusplus — header-only TOML parser; system preferred, FetchContent fallback
# Used by engine/content/ModLoader to parse mod manifests.
# ---------------------------------------------------------------------------
find_package(tomlplusplus 3.4 QUIET)
if(tomlplusplus_FOUND)
    message(STATUS "tomlplusplus: system (${tomlplusplus_VERSION})")
else()
    message(STATUS "tomlplusplus: FetchContent")
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(tomlplusplus)
endif()
