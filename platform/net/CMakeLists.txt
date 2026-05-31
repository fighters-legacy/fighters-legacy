# enet6 networking backend — implements INetwork
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
# enet6 uses add_library() without STATIC/SHARED so it follows BUILD_SHARED_LIBS.
# Force static to avoid an enet6.dll/.so runtime dependency — mirrors the
# openal-soft pattern in dependencies.cmake.
set(_fl_enet_save_bsl "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(enet6)
unset(BUILD_SHARED_LIBS CACHE)
if(_fl_enet_save_bsl)
    set(BUILD_SHARED_LIBS "${_fl_enet_save_bsl}")
endif()
unset(_fl_enet_save_bsl)
unset(CMAKE_COMPILE_WARNING_AS_ERROR)

add_library(platform-enet STATIC ENetNetwork.cpp ENetNetworkFactory.cpp)
target_link_libraries(platform-enet
    PUBLIC  platform-hal
    PRIVATE enet6
)
# enet6 still requires ws2_32/winmm on Windows (same as enet 1.3.x).
if(WIN32)
    target_link_libraries(platform-enet PRIVATE ws2_32 winmm)
endif()
# enet6 target may not propagate its include path; be explicit.
target_include_directories(platform-enet PRIVATE "${enet6_SOURCE_DIR}/include")
