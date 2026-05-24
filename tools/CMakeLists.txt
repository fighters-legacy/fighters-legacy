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

if(TARGET platform-openal)
    add_executable(audio_test audio_test/main.cpp)
    # OpenAL::OpenAL is PRIVATE in platform-openal, so its include dirs don't
    # propagate. audio_test includes OALAudio.h which pulls in <AL/al.h> and
    # <AL/alc.h> — same pattern as hello_triangle adding Vulkan::Vulkan explicitly.
    target_link_libraries(audio_test PRIVATE platform-openal platform-hal OpenAL::OpenAL)
    target_include_directories(audio_test PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/openal"
    )
    if(WIN32)
        add_custom_command(TARGET audio_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:OpenAL::OpenAL>
                $<TARGET_FILE_DIR:audio_test>
        )
    endif()
endif()
