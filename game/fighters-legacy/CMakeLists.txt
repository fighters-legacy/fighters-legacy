if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(fighters-legacy main.cpp)
    target_link_libraries(fighters-legacy PRIVATE
        engine-crash
        engine-config
        engine-firstrun
        engine-content
        engine-sandbox
        engine-loop
        engine-entity
        platform-file-logger
        platform-sdl3
        platform-vulkan
        platform-openal
        platform-hal
        Vulkan::Vulkan
        SDL3::SDL3
        OpenAL::OpenAL)
    # Concrete backend headers (VkRenderer.h, SDL3Window.h) pull in Vulkan/SDL3 system
    # headers that are not on the default search path on all platforms — mirror what
    # hello_triangle does.
    target_include_directories(fighters-legacy PRIVATE
        ${CMAKE_BINARY_DIR}/engine
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
        "${CMAKE_SOURCE_DIR}/platform/sdl3")
    # TODO(Phase 2): add platform-net when game loop wires ENetNetwork
    # On Windows, OpenAL32.dll and SDL3.dll are not in the default search path
    # used by the smoke test — copy both next to the binary so the loader finds them.
    if(WIN32)
        add_custom_command(TARGET fighters-legacy POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:OpenAL::OpenAL>
                $<TARGET_FILE_DIR:fighters-legacy>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3>
                $<TARGET_FILE_DIR:fighters-legacy>
        )
    endif()
endif()
