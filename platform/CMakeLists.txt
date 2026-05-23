# platform-hal: INTERFACE library exposing all HAL interface headers.
# Engine core and backends link against this target rather than including
# platform/ headers by path.
add_library(platform-hal INTERFACE)
target_include_directories(platform-hal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# HAL backends: SDL3 windowing/input, Vulkan renderer, OpenAL Soft audio, ENet networking
add_subdirectory(sdl3)
add_subdirectory(vulkan)
add_subdirectory(openal)
add_subdirectory(net)
