if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(fighters-legacy
        main.cpp
        ClientNetEventHandler.cpp
        CameraInput.cpp
        GameHud.cpp
        HapticController.cpp
        LocalServer.cpp
    )
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
        platform-subprocess
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
        "${enet6_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    # Dev-build server path: embed the build-tree fl-server path at compile time.
    # In release packages fl-server is co-located with fighters-legacy and
    # SDL_GetBasePath() is used instead (see LocalServer.cpp).
    target_compile_definitions(fighters-legacy PRIVATE
        FL_SERVER_DEV_PATH="$<TARGET_FILE_DIR:fl-server>/fl-server")
    if(WIN32)
        target_link_libraries(fighters-legacy PRIVATE ws2_32 winmm)
    endif()
    if(MSVC)
        target_compile_definitions(fighters-legacy PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
