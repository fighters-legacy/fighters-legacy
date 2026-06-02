# Engine core: content system, asset manager, IContentPack interface
# Sources added here as Workstream A progresses

add_library(engine-content STATIC
    content/FolderContentPack.cpp
    content/ModLoader.cpp
    content/AssetManager.cpp
)

target_include_directories(engine-content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-content
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-input STATIC
    input/InputBindings.cpp
    input/AxisConfig.cpp
)

target_include_directories(engine-input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-input
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-i18n STATIC
    i18n/StringTable.cpp
    i18n/Localization.cpp
)

target_include_directories(engine-i18n PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-i18n
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-config STATIC
    config/UserConfig.cpp
)

target_include_directories(engine-config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-config
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
)

add_library(engine-firstrun STATIC
    firstrun/FirstRun.cpp
)

target_include_directories(engine-firstrun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-firstrun
    PUBLIC  engine-config
            platform-hal
)

add_library(engine-audio STATIC
    audio/OggDecoder.cpp
    audio/ogg_impl.c
    audio/SubtitleQueue.cpp
    audio/PlaylistLoader.cpp
    audio/MusicManager.cpp
    audio/VoiceCalloutManager.cpp
)
target_include_directories(engine-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(engine-audio PRIVATE "${stb_SOURCE_DIR}")
# Suppress all warnings in ogg_impl.c — stb_vorbis.c (included inside) has
# many C99 diagnostics that would fail with CMAKE_COMPILE_WARNING_AS_ERROR=ON.
set_source_files_properties(audio/ogg_impl.c PROPERTIES
    COMPILE_OPTIONS  "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>"
    COMPILE_DEFINITIONS "STB_VORBIS_NO_STDIO"
)
target_link_libraries(engine-audio
    PUBLIC  platform-hal
            engine-content
    PRIVATE engine-i18n
            engine-config
            tomlplusplus::tomlplusplus
)

add_library(engine-render STATIC
    render/SimRenderBridge.cpp
    render/CameraController.cpp
    render/SceneRenderer.cpp
    render/ParticleSystem.cpp
    render/BuiltinGeometry.cpp
    render/TerrainChunkIO.cpp
    render/ProceduralTerrainChunk.cpp
    render/TerrainMeshBuilder.cpp
    render/TerrainStreamer.cpp
    render/FlightHud.cpp
)

target_include_directories(engine-render PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(engine-render PRIVATE "${stb_SOURCE_DIR}")

# TerrainChunkIO.cpp defines STB_IMAGE_STATIC which makes all stb symbols TU-local;
# the unused-function warnings on the stb_image.h internal helpers would fail
# -Werror — suppress them with -w, matching the pattern used for ogg_impl.c.
set_source_files_properties(render/TerrainChunkIO.cpp PROPERTIES
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>"
)

target_link_libraries(engine-render
    PUBLIC  platform-hal
    PRIVATE engine-content
            engine-audio
)

add_library(engine-entity STATIC
    entity/EntityPool.cpp
    entity/EntityTypeRegistry.cpp
    entity/EntityDefParser.cpp
    entity/EntityManager.cpp
)

target_include_directories(engine-entity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-entity
    PUBLIC  engine-loop
            platform-hal
    PRIVATE tomlplusplus::tomlplusplus
            engine-render
)

add_library(engine-sandbox STATIC
    sandbox/SandboxInspector.cpp
)

target_include_directories(engine-sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-sandbox
    PUBLIC  platform-hal
            engine-entity
)

add_subdirectory(crash)

add_library(engine-loop STATIC
    loop/GameLoop.cpp
    loop/TimeController.cpp
)
target_include_directories(engine-loop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-loop PUBLIC platform-hal)

add_library(engine-difficulty STATIC
    difficulty/DifficultyMultipliers.cpp
)
target_include_directories(engine-difficulty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-difficulty
    PUBLIC  engine-config
    PRIVATE engine-content
            tomlplusplus::tomlplusplus
)

add_library(engine-flight STATIC
    flight/Atmosphere.cpp
    flight/FlightModelParser.cpp
    flight/AeroForces.cpp
    flight/FlightIntegrator.cpp
)
target_include_directories(engine-flight PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-flight
    PUBLIC  engine-content
    PRIVATE tomlplusplus::tomlplusplus
)

# Authoritative server-sim serializer and network broadcaster.
add_library(engine-net STATIC
    net/WorldBroadcaster.cpp
    net/DiscoveryBeacon.cpp
    net/DiscoveryListener.cpp
)
target_include_directories(engine-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-net
    PUBLIC  engine-entity
            platform-hal
    PRIVATE engine-flight
)
if(WIN32)
    target_link_libraries(engine-net PRIVATE ws2_32)
endif()

# Per-frame stats collection and debug overlay line builder.
add_library(engine-perf STATIC
    perf/PerformanceOverlay.cpp
)
target_include_directories(engine-perf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-perf PUBLIC platform-hal)

# In-game debug console: command registry, drop-down HUD overlay, builtin commands.
add_library(engine-debug STATIC
    debug/DebugCommandRegistry.cpp
    debug/DebugConsole.cpp
    debug/DebugCommands.cpp
)
target_include_directories(engine-debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-debug
    PUBLIC  platform-hal
            engine-entity
    PRIVATE engine-render   # DebugCommands reads SimRenderBridge for the entities command
)
