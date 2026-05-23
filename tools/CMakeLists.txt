# Developer tools
if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(hello_triangle hello_triangle/main.cpp)
    target_link_libraries(hello_triangle
        PRIVATE platform-sdl3 platform-vulkan platform-hal Vulkan::Vulkan
    )
    # Concrete backend headers are not exposed by platform-hal's include path.
    # Vulkan::Vulkan is needed here because main.cpp includes VkRenderer.h which
    # includes <vulkan/vulkan.h> — on Windows this is under %VULKAN_SDK%\Include,
    # which is not a default system search path.
    target_include_directories(hello_triangle PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
    )
endif()
