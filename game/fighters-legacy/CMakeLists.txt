if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(fighters-legacy main.cpp)
    target_link_libraries(fighters-legacy PRIVATE
        engine-crash
        engine-config
        engine-difficulty
        engine-firstrun
        engine-content
        engine-sandbox
        engine-loop
        engine-entity
        engine-net
        engine-weather
        engine-perf
        engine-debug
        platform-file-logger
        platform-sdl3
        platform-vulkan
        platform-openal
        platform-enet
        platform-hal
        Vulkan::Vulkan
        SDL3::SDL3
        OpenAL::OpenAL)
    target_include_directories(fighters-legacy PRIVATE
        ${CMAKE_BINARY_DIR}/engine
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
        "${CMAKE_SOURCE_DIR}/platform/net"
        "${enet6_SOURCE_DIR}/include")
    if(WIN32)
        target_link_libraries(fighters-legacy PRIVATE ws2_32 winmm)
    endif()
    if(MSVC)
        target_compile_definitions(fighters-legacy PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
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
