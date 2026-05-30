# Developer tools
if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(hello_triangle hello_triangle/main.cpp)
    target_link_libraries(hello_triangle
        PRIVATE platform-sdl3 platform-vulkan platform-hal engine-render SDL3::SDL3 Vulkan::Vulkan
    )
    # engine-render: adds the engine/ include root (for render/BuiltinGeometry.h) and
    # transitively links platform-hal + GLM.  engine-content is a PRIVATE dep of
    # engine-render so it does not propagate here.
    # Vulkan::Vulkan: on Windows, <vulkan/vulkan.h> lives under %VULKAN_SDK%\Include
    # which is not a default system search path.
    target_include_directories(hello_triangle PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
    )
endif()

if(TARGET platform-sdl3)
    add_executable(input_check input_check/main.cpp)
    target_link_libraries(input_check PRIVATE platform-sdl3 platform-hal SDL3::SDL3)
    target_include_directories(input_check PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
    )
endif()

if(TARGET platform-openal)
    add_executable(audio_check audio_check/main.cpp)
    target_link_libraries(audio_check PRIVATE platform-openal OpenAL::OpenAL)
    target_include_directories(audio_check PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/openal"
    )
endif()

if(TARGET platform-enet)
    # Config parsing logic in its own library so tests/test_server_config can link it.
    add_library(fl-server-config STATIC fl-server/server_config.cpp)
    target_link_libraries(fl-server-config PUBLIC platform-hal tomlplusplus::tomlplusplus)
    target_include_directories(fl-server-config PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/fl-server"
    )

    add_executable(fl-server fl-server/main.cpp)
    target_link_libraries(fl-server PRIVATE fl-server-config platform-enet)
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

# ---------------------------------------------------------------------------
# Asset pipeline tools — no platform-hal dependency; always built
# ---------------------------------------------------------------------------

add_library(validate-flight-model-lib STATIC
    validate-flight-model/flight_model_validator.cpp)
target_link_libraries(validate-flight-model-lib PUBLIC tomlplusplus::tomlplusplus)
target_include_directories(validate-flight-model-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-flight-model")

add_executable(validate-flight-model validate-flight-model/main.cpp)
target_link_libraries(validate-flight-model PRIVATE validate-flight-model-lib)

add_library(validate-mission-lib STATIC validate-mission/mission_validator.cpp)
target_link_libraries(validate-mission-lib PUBLIC yaml-cpp::yaml-cpp)
target_include_directories(validate-mission-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-mission")

add_executable(validate-mission validate-mission/main.cpp)
target_link_libraries(validate-mission PRIVATE validate-mission-lib)

add_library(validate-licenses-lib STATIC validate-licenses/license_validator.cpp)
target_link_libraries(validate-licenses-lib PUBLIC tomlplusplus::tomlplusplus)
target_include_directories(validate-licenses-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-licenses")

add_executable(validate-licenses validate-licenses/main.cpp)
target_link_libraries(validate-licenses PRIVATE validate-licenses-lib)

add_library(validate-mesh-lib STATIC validate-mesh/mesh_validator.cpp)
target_include_directories(validate-mesh-lib SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-mesh"
    "${tinygltf_SOURCE_DIR}")
if(TARGET tinygltf)
    target_link_libraries(validate-mesh-lib PUBLIC tinygltf)
endif()

add_executable(validate-mesh validate-mesh/main.cpp)
target_link_libraries(validate-mesh PRIVATE validate-mesh-lib)

add_library(tex-compress-lib STATIC tex-compress/tex_compress.cpp)
target_include_directories(tex-compress-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tex-compress")

add_executable(tex-compress tex-compress/main.cpp)
target_link_libraries(tex-compress PRIVATE tex-compress-lib)

if(MSVC)
    target_compile_definitions(tex-compress     PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(tex-compress-lib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
