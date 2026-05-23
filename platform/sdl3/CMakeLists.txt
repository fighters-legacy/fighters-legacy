# SDL3 windowing and input backend — implements IWindow
if(NOT SDL3_FOUND)
    # CMAKE_COMPILE_WARNING_AS_ERROR from our debug preset propagates to all
    # FetchContent targets. SDL 3.2.x has sign-compare warnings in its libm
    # source that become errors on newer compilers. Disable for the SDL build
    # only; unset restores the cache value (ON) for our own targets below.
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    FetchContent_MakeAvailable(SDL3)
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)
endif()

add_library(platform-sdl3 STATIC SDL3Window.cpp)
target_link_libraries(platform-sdl3
    PUBLIC  platform-hal
    PRIVATE SDL3::SDL3
)
