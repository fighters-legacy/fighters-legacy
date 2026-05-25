if(TARGET platform-sdl3 AND TARGET platform-vulkan)
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
        platform-hal
        Vulkan::Vulkan
        SDL3::SDL3)
    # Concrete backend headers (VkRenderer.h, SDL3Window.h) pull in Vulkan/SDL3 system
    # headers that are not on the default search path on all platforms — mirror what
    # hello_triangle does.
    target_include_directories(fighters-legacy PRIVATE
        ${CMAKE_BINARY_DIR}/engine
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
        "${CMAKE_SOURCE_DIR}/platform/sdl3")
    # TODO(Phase 2): add platform-net when game loop wires ENetNetwork
endif()
