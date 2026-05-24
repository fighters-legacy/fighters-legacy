# OpenAL Soft audio backend — implements IAudio
if(NOT OPENAL_FOUND AND NOT OpenAL_FOUND)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    # Force static library: avoids .dll/.so deployment for tests and tools on all
    # platforms, especially Windows where the DLL would otherwise need to be
    # alongside every test/tool binary.
    set(_saved_bsl ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(openal-soft)
    set(BUILD_SHARED_LIBS ${_saved_bsl})
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)
endif()

add_library(platform-openal STATIC OALAudio.cpp)
target_link_libraries(platform-openal
    PUBLIC  platform-hal
    PRIVATE OpenAL::OpenAL
)
