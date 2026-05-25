add_executable(fighters-legacy main.cpp)
target_link_libraries(fighters-legacy PRIVATE
    engine-crash
    engine-config
    engine-firstrun
    engine-content
    platform-file-logger
    platform-sdl3
    platform-vulkan
    platform-openal
    platform-hal)
target_include_directories(fighters-legacy PRIVATE ${CMAKE_BINARY_DIR}/engine)
# TODO(Phase 2): add platform-net when game loop wires ENetNetwork
