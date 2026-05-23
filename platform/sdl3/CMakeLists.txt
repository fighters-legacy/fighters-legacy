# SDL3 windowing and input backend — implements IWindow
if(NOT SDL3_FOUND)
    FetchContent_MakeAvailable(SDL3)
endif()

add_library(platform-sdl3 STATIC SDL3Window.cpp)
target_link_libraries(platform-sdl3
    PUBLIC  platform-hal
    PRIVATE SDL3::SDL3
)
