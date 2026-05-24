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

if(TARGET platform-sdl3)
    add_executable(input_test input_test/main.cpp)
    target_link_libraries(input_test PRIVATE platform-sdl3 platform-hal SDL3::SDL3)
    target_include_directories(input_test PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
    )
endif()

if(TARGET platform-enet)
    add_executable(fl-server fl-server/main.cpp)
    target_link_libraries(fl-server PRIVATE platform-enet platform-hal tomlplusplus::tomlplusplus)
    target_include_directories(fl-server PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
        "${enet_SOURCE_DIR}/include"
    )
endif()
