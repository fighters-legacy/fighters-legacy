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

    add_executable(fl-client fl-client/main.cpp)
    target_link_libraries(fl-client PRIVATE platform-enet platform-hal)
    target_include_directories(fl-client PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
        "${enet_SOURCE_DIR}/include"
    )

    # MSVC deprecates std::getenv — suppress for these tools only
    if(MSVC)
        target_compile_definitions(fl-server PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_definitions(fl-client PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

add_executable(locale-extract locale-extract/main.cpp)
target_link_libraries(locale-extract PRIVATE tomlplusplus::tomlplusplus)
# No platform-hal dependency — uses std::filesystem + std::regex directly.

# Optional: compile-time key constants header (demand-driven, not built by default)
set(LOCALE_KEYS_HEADER "${CMAKE_BINARY_DIR}/generated/i18n/LocaleKeys.h")
add_custom_command(
    OUTPUT "${LOCALE_KEYS_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated/i18n"
    COMMAND $<TARGET_FILE:locale-extract>
            --src "${CMAKE_SOURCE_DIR}"
            --locale "${CMAKE_SOURCE_DIR}/locale"
            --gen-keys "${LOCALE_KEYS_HEADER}"
    DEPENDS locale-extract
    COMMENT "Generating i18n key constants"
    VERBATIM
)
add_custom_target(locale-keys DEPENDS "${LOCALE_KEYS_HEADER}")
# Usage: add_dependencies(my-target locale-keys)
#        target_include_directories(my-target PRIVATE "${CMAKE_BINARY_DIR}/generated")
