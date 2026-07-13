# Developer tools
if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    add_executable(hello_triangle hello_triangle/main.cpp)
    target_link_libraries(hello_triangle
        PRIVATE platform-sdl3 platform-vulkan platform-hal engine-render Vulkan::Vulkan
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
    target_link_libraries(input_check PRIVATE platform-sdl3 platform-hal)
    target_include_directories(input_check PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/sdl3"
    )
endif()

if(TARGET platform-openal)
    add_executable(audio_check audio_check/main.cpp)
    target_link_libraries(audio_check PRIVATE platform-openal engine-audio)
    target_include_directories(audio_check PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/openal"
    )
endif()

add_library(validate-playlist-lib STATIC
    validate-playlist/playlist_validator.cpp)
target_link_libraries(validate-playlist-lib PUBLIC tomlplusplus::tomlplusplus)
target_include_directories(validate-playlist-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-playlist")

add_executable(validate-playlist validate-playlist/main.cpp)
target_link_libraries(validate-playlist PRIVATE validate-playlist-lib)

if(TARGET platform-enet)
    add_executable(net_check net_check/main.cpp)
    # engine-protocol PUBLIC-exposes the engine/ include root (NetStats.h -> perf/Stats.h) without
    # pulling engine-net / engine-entity.
    target_link_libraries(net_check PRIVATE platform-enet platform-hal engine-protocol)
    target_include_directories(net_check PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
    )

    if(MSVC)
        target_compile_definitions(net_check PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # bot_swarm: headless multi-client load generator. Pure client (connects to an
    # existing fl-server); reuses the wire protocol via engine-protocol (zero-dep — no
    # WorldBroadcaster / engine-entity) + the platform-net transport facade.
    # Links platform-net (not platform-enet directly) since #649: the swarm still DEFAULTS to
    # enet6 — it remains the enet6 regression instrument — but --transport gns lets it exercise
    # the default internet transport, which was otherwise never scale-tested. In an
    # FL_ENABLE_GNS=OFF build, platform-net is enet6-only and bot_swarm refuses --transport gns.
    # No SDL/Vulkan/OpenAL deps — builds headless in CI.
    find_package(Threads REQUIRED)
    add_executable(bot_swarm
        bot_swarm/main.cpp
        bot_swarm/BotClient.cpp
        bot_swarm/LossyProxy.cpp
    )
    target_link_libraries(bot_swarm PRIVATE platform-net platform-hal engine-protocol Threads::Threads)
    target_include_directories(bot_swarm PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/bot_swarm"
    )
    if(WIN32)
        # winmm: timeBeginPeriod; ws2_32: the LossyProxy raw UDP relay (#714).
        target_link_libraries(bot_swarm PRIVATE winmm ws2_32)
    endif()
    if(MSVC)
        target_compile_definitions(bot_swarm PRIVATE _CRT_SECURE_NO_WARNINGS)
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

# Schema validity is delegated to the engine's own parser (engine-weapon), so the validator and the
# runtime cannot drift. engine-weapon is pure stdlib + toml++ — it pulls in no platform-hal, so this
# stays an always-built asset tool.
add_library(validate-weapon-lib STATIC
    validate-weapon/weapon_validator.cpp)
target_link_libraries(validate-weapon-lib
    PUBLIC tomlplusplus::tomlplusplus
    PRIVATE engine-weapon)
target_include_directories(validate-weapon-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-weapon")

add_executable(validate-weapon validate-weapon/main.cpp)
target_link_libraries(validate-weapon PRIVATE validate-weapon-lib)

# Same shape as validate-weapon: the schema lives in engine-sensor's parser, and this tool only adds
# the plausibility checks a runtime parser must not make.
add_library(validate-sensor-lib STATIC
    validate-sensor/sensor_validator.cpp)
target_link_libraries(validate-sensor-lib
    PRIVATE engine-sensor)
target_include_directories(validate-sensor-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/validate-sensor")

add_executable(validate-sensor validate-sensor/main.cpp)
target_link_libraries(validate-sensor PRIVATE validate-sensor-lib)

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

# terrain-chunk-io: encode/decode terrain chunks and generate procedural chunks.
# Unconditional — no Vulkan or SDL3 dependency required.
add_executable(terrain-chunk-io terrain-chunk-io/main.cpp)
target_link_libraries(terrain-chunk-io PRIVATE engine-render)
