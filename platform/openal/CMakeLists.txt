# OpenAL Soft audio backend — implements IAudio
# On Apple, find_package(OpenAL) finds the deprecated system OpenAL.framework
# which provides <OpenAL/al.h>, not <AL/al.h>. Always use FetchContent on Apple
# to ensure we get OpenAL Soft with the correct include layout.
if(NOT (OPENAL_FOUND OR OpenAL_FOUND) OR APPLE)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    # Force static library: avoids .dll/.so deployment for tests and tools on all
    # platforms, especially Windows where the DLL would otherwise need to be
    # alongside every test/tool binary.
    set(_saved_bsl "${BUILD_SHARED_LIBS}")
    # Must use CACHE FORCE: option() in openal-soft's CMakeLists ignores a plain
    # set() and would create a cache entry with its default (ON = shared DLL).
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(openal-soft)
    # Restore: remove the forced cache entry so it doesn't affect other targets.
    unset(BUILD_SHARED_LIBS CACHE)
    if(_saved_bsl)
        set(BUILD_SHARED_LIBS "${_saved_bsl}")
    endif()
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)
endif()

add_library(platform-openal STATIC OALAudio.cpp)
target_link_libraries(platform-openal
    PUBLIC  platform-hal
    PRIVATE OpenAL::OpenAL
)
