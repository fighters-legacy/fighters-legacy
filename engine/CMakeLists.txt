# Engine core: content system, asset manager, IContentPack interface
# Sources added here as Workstream A progresses

add_library(engine-content STATIC
    content/FolderContentPack.cpp
    content/ModLoader.cpp
    content/AssetManager.cpp
    content/AssetValidator.cpp
    content/BundledBaseTerrain.cpp
    content/ContentIndex.cpp
)

target_include_directories(engine-content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine-content
    PUBLIC  platform-hal
    PRIVATE tomlplusplus::tomlplusplus
            ${CMAKE_DL_LIBS}   # dlopen on Linux; empty on Windows/macOS (auto-linked)
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
    config/ConfigFile.cpp
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
    audio/SubtitleQueue.cpp
    audio/PlaylistLoader.cpp
    audio/MusicManager.cpp
    audio/VoiceCalloutManager.cpp
    audio/SfxBuiltinSounds.cpp
    audio/SfxManager.cpp
)
target_include_directories(engine-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-audio
    PUBLIC  platform-hal
            engine-content
    PRIVATE engine-i18n
            engine-config
            tomlplusplus::tomlplusplus
            vorbisfile
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
    render/WindshieldRain.cpp
)

target_include_directories(engine-render PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(engine-render PRIVATE "${stb_SOURCE_DIR}")

# TerrainChunkIO.cpp defines STB_IMAGE_STATIC which makes all stb symbols TU-local;
# the unused-function warnings on the stb_image.h internal helpers would fail
# -Werror — suppress them with -w for this one vendored-header TU.
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
    entity/DamageApplication.cpp
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
    flight/BallisticForceModel.cpp
    flight/FixedWingForceModel.cpp
    flight/Trim.cpp
)
target_include_directories(engine-flight PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-flight
    PUBLIC  engine-content
    PRIVATE tomlplusplus::tomlplusplus
)

# Weapon definitions (content-pack weapon TOML), the id-keyed registry, and the loadout -> payload
# summation (#812).
#
# The DEPENDENCY DIRECTION is deliberate and one-way: engine-weapon knows about entities and flight,
# not the reverse. engine-entity still does not depend on engine-weapon -- a hardpoint names its
# stores as plain id strings, so an airframe describes what it can carry without the entity system
# knowing anything about weapons. Loadout.cpp is the one place the two meet, and it lives on this
# side of the line because turning stores into mass and drag is a WEAPON question, not an entity one.
add_library(engine-weapon STATIC
    weapon/WeaponDefParser.cpp
    weapon/WeaponRegistry.cpp
    weapon/Loadout.cpp
    weapon/FireControl.cpp
    weapon/ProjectileSystem.cpp
    weapon/Seeker.cpp
)
target_include_directories(engine-weapon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-weapon
    PUBLIC  engine-flight   # PayloadEffect is in the defaultPayload() signature
            platform-hal    # GLM for the projectile point-mass math (#625)
    PRIVATE engine-entity   # EntityDef::hardpoints, read only; the projectile pool mirrors entities
            engine-spatial  # contact-fuze broadphase
            engine-sensor   # the seeker runs the shared Detection.h machine (#627)
            tomlplusplus::tomlplusplus
)

# Content bootstrap: turning a pack's files into the registries the sim and the UI read (#683/#810/#812).
# Promoted out of fl-server because the GAME needs it too -- the in-game manual (#821) resolves an
# aircraft's stations and sensors from the same pack, and a second copy of this logic is a second
# chance for the client and the server to disagree about what an aircraft carries.
add_library(engine-bootstrap STATIC
    content/ContentBootstrap.cpp
)
target_include_directories(engine-bootstrap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-bootstrap
    PUBLIC  engine-content
    PRIVATE engine-entity engine-weapon engine-sensor
)

# The in-game aircraft manual (#821). It is a CONSUMER of the same sources of truth as the sim --
# fl::trim() for performance (the same function fm-trim's CI gate uses), [aero.limits] for limits,
# EntityDef::hardpoints + the WeaponRegistry for stores, and resolved SensorDefs for the sensor suite.
# Nothing in it is authored twice, so it cannot drift from the aircraft the player is flying.
add_library(engine-manual STATIC
    manual/AircraftManual.cpp
)
target_include_directories(engine-manual PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-manual
    PUBLIC  engine-flight
    PRIVATE engine-entity engine-weapon engine-sensor
)

# Sensor definitions (content-pack sensor TOML). One vocabulary, three consumers: player avionics,
# AI detection, and missile seekers all read the same SensorDef (2026-07-12 decision record).
#
# PUBLIC engine-entity (#684): the detection math reads a target's SignatureDef, and picks up GLM
# through engine-entity's own PUBLIC platform-hal edge. Note the direction — engine-sensor depends on
# engine-entity and NOT the reverse: the observed must not depend on the observer, which is why
# SignatureDef is a POD in engine/entity/ rather than a type in this library.
#
# PUBLIC engine-spatial (#685): the sensing pass queries the index for detection candidates. Both
# PUBLIC edges from the decision record are now in place, and both point AWAY from engine-sensor's
# consumers — engine-net links this, not the other way round.
add_library(engine-sensor STATIC
    sensor/SensorDefParser.cpp
    sensor/Detection.cpp
    sensor/SensorSystem.cpp
)
target_include_directories(engine-sensor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-sensor
    PUBLIC  engine-entity
            engine-spatial
    PRIVATE tomlplusplus::tomlplusplus
)

add_subdirectory(ai)

# World-systems foundation types (faction registry, alert levels, airspace zones).
add_subdirectory(world)

# Server-authoritative weather state machine, time-of-day, and wind/gust model.
add_library(engine-weather STATIC
    weather/WeatherController.cpp
)
target_include_directories(engine-weather PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-weather PUBLIC platform-hal)

add_library(engine-spatial STATIC
    spatial/SpatialIndex.cpp
)
target_include_directories(engine-spatial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# No external deps — pure ISO C++20 stdlib.

# Mission runtime: the mission YAML schema owner (parser + runtime model, MissionParser) and the
# faction-aware sim-setup applier (MissionSetup). Lives in the ENGINE, not fl-server, because the game
# client needs the same runtime for the single-player mission lifecycle (#634) and Instant Action
# (#40); fl-server just wires it at startup (#854) -- the same promotion rationale as engine-bootstrap.
# validate-mission delegates its schema to parseMission here, so the linter and the engine cannot
# drift (the rule shared by validate-weapon/-sensor/-entity). Reaches no client backend.
add_library(engine-mission STATIC
    mission/MissionParser.cpp
    mission/MissionSetup.cpp
    mission/MissionRuntime.cpp
)
target_include_directories(engine-mission PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-mission
    PUBLIC  engine-entity   # EntityManager/EntityState + GLM (its platform-hal edge); LocalFrame/Geodetic headers
            engine-world    # FactionRegistry — coalition setup
            engine-weather  # WeatherController + WeatherPreset (WeatherTypes.h is in Mission.h)
    PRIVATE yaml-cpp::yaml-cpp
)

# Data-parallel task scheduler: persistent worker pool + blocking parallel_for. Declared before
# engine-net so the broadcaster can link it for the parallel sim tick.
add_library(engine-job STATIC
    job/JobSystem.cpp
)
target_include_directories(engine-job PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# No external deps — pure ISO C++20 stdlib (threads via the standard library).
find_package(Threads REQUIRED)
target_link_libraries(engine-job PUBLIC Threads::Threads)

# Zero-dependency wire protocol: naturally-aligned wire types (GameProtocol.h), the bit-packed
# snapshot codec (SnapshotCodec + BitStream + Quantization), and the selective-ack window
# (AckWindow.h). Consumed by the server sim, the game client, and headless tools WITHOUT pulling
# WorldBroadcaster / engine-entity. Reaches only the C++ stdlib — enforced at configure time by
# fl_assert_layering() (cmake/layering.cmake, deferred from the root CMakeLists.txt).
add_library(engine-protocol STATIC
    net/SnapshotCodec.cpp
)
target_include_directories(engine-protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Engine-layer snapshot payload compression (#775). Deliberately NOT part of engine-protocol —
# that target is stdlib-only (layering rule 2, cmake/layering.cmake); the zstd dependency hangs
# off this target instead, and both the server (via engine-net) and the game client link it.
add_library(engine-compress STATIC
    net/SnapshotCompression.cpp
)
target_include_directories(engine-compress PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-compress PRIVATE fl::zstd)

# Authoritative server-sim serializer and network broadcaster.
add_library(engine-net STATIC
    net/NetworkUtils.cpp
    net/WorldBroadcaster.cpp
    net/DiscoveryBeacon.cpp
    net/DiscoveryListener.cpp
    net/SnapshotScheduler.cpp
    net/CongestionController.cpp
    net/TickGovernor.cpp
)
target_include_directories(engine-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-net
    PUBLIC  engine-protocol
            engine-compress
            engine-entity
            engine-weapon    # the fire path: FireControl + ProjectileSystem (#625)
            engine-sensor
            engine-spatial
            engine-job
            engine-world
            platform-hal
    PRIVATE engine-flight
            engine-weather
)
if(WIN32)
    target_link_libraries(engine-net PRIVATE ws2_32)
endif()

# Per-frame stats collection and debug overlay line builder.
add_library(engine-perf STATIC
    perf/PerformanceOverlay.cpp
    perf/ProcessStats.cpp
)
target_include_directories(engine-perf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-perf PUBLIC platform-hal)
if(WIN32)
    target_link_libraries(engine-perf PUBLIC psapi) # GetProcessMemoryInfo (ProcessStats)
endif()

# Console subsystem: command registry, builtin commands, base shell, in-game HUD overlay.
add_library(engine-console STATIC
    console/CommandRegistry.cpp
    console/ConsoleCommands.cpp
    console/CommandShell.cpp
    console/GameConsole.cpp
)
target_include_directories(engine-console PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-console
    PUBLIC  platform-hal
            engine-entity
    PRIVATE engine-render   # ConsoleCommands reads SimRenderBridge for the entities command
            engine-weather  # ConsoleCommands calls WeatherController::setPreset
)

# Sandboxed Lua 5.5 execution environment for AI and mission scripts.
add_library(engine-script STATIC
    script/LuaSandbox.cpp
    script/LuaController.cpp
)
target_include_directories(engine-script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-script
    PUBLIC  platform-hal
            engine-entity      # IEntityController, EntityState, EntityManager
    PRIVATE lua::lua
            engine-ai          # Guidance.h math for LuaController bindings
            engine-sensor      # detected_contacts() reads AiTickContext::contacts (#691)
)
