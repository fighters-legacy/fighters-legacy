# Test suite — Catch2 via cmake/dependencies.cmake
include(CTest)
include(Catch)

# Tests are registered here as engine code lands, one line each (#1265):
#   fl_add_test(test_foo LIBS engine-content)
#   fl_add_test(test_foo LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
#   fl_add_test(test_foo LIBS engine-loop SERIAL)   # only for wall-clock-timing tests
#
# fl_add_test lives in cmake/fl_helpers.cmake: it builds <name> from <name>.cpp, always links
# Catch2::Catch2WithMain, and calls catch_discover_tests. It replaced 188 hand-written four-line
# copies of that shape, two of which had already drifted into registering their test from inside
# the NEXT test's block. Add a new test next to TOPICALLY RELATED ones rather than at one common
# anchor — a shared insertion point is what makes every concurrent PR conflict here.
#
# Anything genuinely different — the mission-harness add_test blocks, test_audio's Windows DLL copy,
# targets taking FL_SERVER_BIN — stays written out longhand below.

fl_add_test(test_renderer LIBS platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# MeshNodePlan (#839/#882): the glTF scene-graph walk + variant node-set filter + articulation-pose
# composition behind node-aware mesh loading. GPU-free by construction, so it runs in every CI leg.
fl_add_test(test_mesh_node_plan LIBS platform-meshgraph INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# MeshArticulation (#840): the channel registry, clip parser and scrub sampler. Engine-side and
# GPU-free -- the rig is parsed from the same .glb bytes the renderer uploads.
fl_add_test(test_mesh_articulation LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_mesh_articulation_paths LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_window LIBS platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_bridge LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_scene_renderer LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# StdFilesystemWatcher (#152): the polling hot-reload backend.
fl_add_test(test_std_filesystem_watcher LIBS platform-stdfs INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# PreviewScene (#666): the authoring-tools single-model preview bootstrap. engine-content is a PRIVATE
# dep of engine-render, so link it explicitly for AssetManager in the test.
fl_add_test(test_preview_scene LIBS engine-render engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# fl-viewer option parsing (#666) + overlay gizmos (#838) — only when the tool's lib is built.
if(TARGET fl-viewer-lib)
    add_executable(test_viewer_options test_viewer_options.cpp)
    target_link_libraries(test_viewer_options PRIVATE Catch2::Catch2WithMain fl-viewer-lib)
    target_include_directories(test_viewer_options PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_viewer_options)

    add_executable(test_overlay_gizmos test_overlay_gizmos.cpp)
    target_link_libraries(test_overlay_gizmos PRIVATE Catch2::Catch2WithMain fl-viewer-lib)
    target_include_directories(test_overlay_gizmos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_overlay_gizmos)
endif()

# The one HudElement assembler behind FlightHud, GameConsole and the screens (#1261).
fl_add_test(test_hud_builder LIBS platform-hal INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_flight_hud LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_windshield_rain LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_particle_system LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_entity LIBS engine-entity engine-spatial INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Terrain line-of-sight segment query (#687): header-only, pure engine-spatial (no glm).
fl_add_test(test_los LIBS engine-spatial INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# World-to-HUD projection helper (#692): pure math, checked against CameraController::view().
fl_add_test(test_hud_projection LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Player autopilot (#640): pure hold-controller + closed-loop convergence through the integrator.
add_executable(test_autopilot test_autopilot.cpp)
target_link_libraries(test_autopilot PRIVATE Catch2::Catch2WithMain engine-flight engine-ai game-client)
target_include_directories(test_autopilot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_autopilot)

# Turn-law audit (#1143): every controller that commands a roll, flown through the real integrator
# in a scenario that keeps its heading error alive. Station-keeping controllers must stay upright;
# manoeuvre controllers must still be able to roll hard, pinned so nobody "consistency"-fixes them.
fl_add_test(test_ai_turn_law LIBS engine-flight engine-ai engine-entity engine-spatial engine-sensor)

# AI altitude hold (#1141): a LoiterController flown through the real integrator. Nothing asserted
# that an AI aircraft asked to hold an altitude actually does, which is how a several-hundred-metre
# oscillation went unnoticed until it started flying the load-spawn world into the ground.
# LSO + crew-chief voice keys (#1108): a pack records OGGs against these names, so the strings are
# pinned rather than incidental.
fl_add_test(test_crew_phrases LIBS engine-atc)

fl_add_test(test_ai_altitude_hold
    LIBS engine-flight engine-ai engine-entity engine-spatial engine-sensor
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Orbit geometry (#1340): the loiter controllers must fly the circle they were given, and must not
# spiral outward when the commanded radius is below what the airframe can turn at its own speed.
fl_add_test(test_ai_orbit
    LIBS engine-flight engine-ai engine-entity engine-spatial engine-sensor
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# The terrain floor (#1352): a hard deck expressed in MSL protects nothing over terrain, and the
# C++ controllers had no deck at all. Closed-loop, because the failure is a trajectory.
fl_add_test(test_ai_terrain_floor
    LIBS engine-flight engine-ai engine-entity engine-spatial engine-sensor
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Client-side target designation (#696): pure snapshot cycling / filtering / best-in-cone.
add_executable(test_target_designation test_target_designation.cpp)
target_link_libraries(test_target_designation PRIVATE Catch2::Catch2WithMain engine-render engine-entity game-client)
target_include_directories(test_target_designation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_target_designation)

# Padlock tracker (#697): pure aim + lock state machine, scripted trajectories.
# Wingman brevity voice keys (#1108). Links game-client: the wingman's text and keys are CLIENT-side
# by policy (the server sends a result code, never a string).
fl_add_test(test_wingman_brevity LIBS engine-render engine-entity game-client INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_padlock_tracker test_padlock_tracker.cpp)
target_link_libraries(test_padlock_tracker PRIVATE Catch2::Catch2WithMain engine-spatial platform-hal game-client)
target_include_directories(test_padlock_tracker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_padlock_tracker)

# Target-slaved inset camera math (#698): pure standoff / look-at / rect layout.
add_executable(test_inset_view test_inset_view.cpp)
target_link_libraries(test_inset_view PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_inset_view PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy" ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_inset_view)

# Head tracking (#927): pure opentrack parse + EMA/freshness filter (socketless).
add_executable(test_head_tracker test_head_tracker.cpp)
target_link_libraries(test_head_tracker PRIVATE Catch2::Catch2WithMain engine-config platform-hal game-client)
target_include_directories(test_head_tracker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(test_head_tracker PRIVATE ws2_32)
endif()
catch_discover_tests(test_head_tracker)

# Ejection / pilot survival model (#672): pure envelope + territory -> outcome logic (header-only).
fl_add_test(test_ejection LIBS engine-entity)

fl_add_test(test_atmosphere LIBS engine-flight)

fl_add_test(test_table2d LIBS engine-flight)

fl_add_test(test_flight_model_parser LIBS engine-flight)

fl_add_test(test_weapon_def_parser LIBS engine-weapon)

# TransformHistory (#425): the lag-compensation rewind ring (header-only, engine-net).
# Entity-entity collision geometry + damage helpers (#630).
fl_add_test(test_collision LIBS engine-entity)

# Per-subsystem damage routing (#675) — the pure pick/apply logic.
fl_add_test(test_subsystem_damage LIBS engine-entity)

fl_add_test(test_transform_history LIBS engine-net)

# Required-pack policy (#872): the pure spec/policy parsing + missing-pack comparison (header-only).
fl_add_test(test_required_pack_policy LIBS engine-net)

# FireControl + ProjectileSystem (#625): the pure fire-decision logic and the 3-DOF projectile pool.
# engine-weapon's entity/spatial deps are PRIVATE, so the test links them explicitly.
fl_add_test(test_fire_control LIBS engine-weapon engine-entity engine-spatial)

# Turret mounts (#970): the pure slew-servo + pose math (stepTurret, turretWorldDir).
fl_add_test(test_turret LIBS engine-weapon)

# Crewed control frame (#969): per-seat sampling + turret slew + per-seat fire through a real WB tick.
fl_add_test(test_crew_frame
    LIBS engine-net engine-entity engine-weapon engine-spatial engine-weather engine-job
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Bot turret gunner + per-instance skill (#971): honest-sensing gunnery through a real WB tick.
fl_add_test(test_turret_gunner
    LIBS engine-net engine-ai engine-sensor engine-entity engine-weapon engine-spatial engine-weather engine-job
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# SeekerTrack + PN guidance (#627): the missile seeker over the shared Detection.h machine.
fl_add_test(test_seeker LIBS engine-weapon engine-entity engine-sensor engine-spatial)

# Chaff/flare countermeasure decoys + seeker seduction (#529).
fl_add_test(test_countermeasure_system LIBS engine-weapon engine-entity engine-sensor engine-spatial)

# Bombs/rockets release ballistics + CCIP (#629).
fl_add_test(test_release_ballistics LIBS engine-weapon engine-entity engine-spatial)

# Ballistic missile guidance + the MIRV controller-spawn seam (#355).
fl_add_test(test_ballistic_guidance
    LIBS engine-ai engine-net engine-entity engine-flight engine-bootstrap
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

# WeaponRegistry + defaultPayload (#812): a loadout that actually costs the airframe mass and drag.
# fm-trim (#817): the tests LOCK the derived numbers, so they double as the engine's guard against
# someone changing AeroForces.cpp and silently altering every aircraft's performance.
# The generated aircraft manual (#821): retuning the flight model must move the manual.
fl_add_test(test_aircraft_manual LIBS engine-manual engine-entity engine-weapon engine-sensor)

fl_add_test(test_fm_trim LIBS fm-trim-lib)

fl_add_test(test_loadout LIBS engine-weapon engine-entity engine-flight)

fl_add_test(test_sensor_def_parser LIBS engine-sensor)

fl_add_test(test_sensor_detection LIBS engine-sensor)

# #670 acceptance: honest sensing end-to-end through a real WorldBroadcaster tick.
fl_add_test(test_honest_sensing
    LIBS engine-net engine-ai engine-sensor engine-entity engine-spatial engine-weather engine-job)

# Builtin surface entities + SAM/AAA engagement controllers (#863): a WB-level engagement test, so it
# links engine-ai (the controllers) + engine-bootstrap (the builtin surface entity defs).
fl_add_test(test_surface_threats
    LIBS engine-net engine-ai engine-sensor engine-bootstrap engine-entity engine-spatial engine-weather engine-job)

fl_add_test(test_sensor_system LIBS engine-sensor engine-entity engine-spatial)

fl_add_test(test_aero_forces LIBS engine-flight)

# The one engine-failure decode the four force models share (#1258). EngineFailFlags.h is
# stdlib-only, so this needs no engine library.
fl_add_test(test_engine_fail_flags INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_flight_integrator LIBS engine-flight)

# The one angle-of-attack formula the HUD and both haptic paths share (#1251). Header-only, but it
# takes glm types, so it links engine-flight for the include surface.
fl_add_test(test_air_angles LIBS engine-flight)

# ArticulationChannels (#1195): the one FlightState -> ArtChannel mapping the wire codec and the
# renderer share. engine-flight only — the mapping deliberately pulls in nothing but the
# dependency-free channel vocabulary.
fl_add_test(test_articulation_channels LIBS engine-flight)

fl_add_test(test_rotor_force_models LIBS engine-flight)
fl_add_test(test_builtin_models LIBS engine-bootstrap engine-flight)

fl_add_test(test_carrier_deck LIBS engine-net engine-entity engine-flight engine-bootstrap)

fl_add_test(test_base_ops LIBS engine-net engine-entity engine-flight engine-weapon engine-bootstrap)

fl_add_test(test_central_gravity LIBS engine-flight)

add_executable(test_geodetic test_geodetic.cpp)
# engine-world for SandboxHome.h — the anchor cases assert against the shipped home (#1211).
target_link_libraries(test_geodetic PRIVATE Catch2::Catch2WithMain engine-flight engine-world)
catch_discover_tests(test_geodetic)

# Local-level (ENU tangent-plane) frame utilities (#470); header-only, glm via engine-flight.
fl_add_test(test_local_frame LIBS engine-flight)

# Cube-sphere tile addressing + geometry core (#469); header-only, needs glm (engine-render) +
# Geodetic.h (engine/flight, header-only).
fl_add_test(test_cube_sphere LIBS engine-render INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_force_model LIBS engine-flight)

# SERIAL: test_loop contains WALL-CLOCK RATE assertions (it sleeps 1 s and asserts the loop fired
# ~60 times). Since #787 made the suite parallel by default, those ran while ~20 other test binaries
# saturated the CPU — so they were no longer measuring the game loop, they were measuring how busy the
# runner was, and they flaked on shared macOS runners. Serialising them restores what the assertion is
# actually for. It costs a few seconds of wall time; a rate test that competes for the CPU is worthless.
fl_add_test(test_loop LIBS engine-loop INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} SERIAL)

fl_add_test(test_sandbox LIBS engine-sandbox INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_content_system LIBS engine-content engine-difficulty engine-sensor)

fl_add_test(test_folder_content_pack LIBS engine-content)

# fl::tomlInt guards five parsers against a real UB bug in toml++ (#824).
# The one strict/tolerant number parse family behind the console, admin, AI-factory, importer and
# mission-runtime parsers (#1244). Header-only, so no engine library is needed.
fl_add_test(test_parse INCLUDES "${CMAKE_SOURCE_DIR}/engine")

# The one quaternion rotate family (#1248). Header-only; the deck-path case pins bit-identity with
# the expression it replaced.
add_executable(test_quat test_quat.cpp)
target_link_libraries(test_quat PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_quat PRIVATE "${CMAKE_SOURCE_DIR}/engine")
# The bit-identity case compares the shared helper against the expression it replaced. FMA
# contraction can fuse a multiply-add on one side and not the other -- Apple Clang at -O2 does, and
# the two land ~2 ulp apart -- which tests the code generator rather than the algebra. Contraction
# is a rounding difference; REASSOCIATION is the defect this guards against, so switch it off.
if(NOT MSVC)
    target_compile_options(test_quat PRIVATE -ffp-contract=off)
endif()
catch_discover_tests(test_quat)

# The one FNV-1a, checked against the PUBLISHED vectors (#1247) -- two copies had a basis one digit
# short and agreed with themselves perfectly, so only an external reference catches that class of bug.
fl_add_test(test_fnv INCLUDES "${CMAKE_SOURCE_DIR}/engine")

# The shared angle/unit constants (#1246). Header-only, so this needs no engine library -- it pins
# that every spelling the sweep replaced is bit-identical, which is the claim that let it land
# without a determinism re-bless.
fl_add_test(test_math_constants INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_rate_window INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_toml_numeric LIBS tomlplusplus::tomlplusplus INCLUDES "${CMAKE_SOURCE_DIR}/engine")

# The shared required/optional TOML field reads behind the seven def parsers (#1245).
fl_add_test(test_toml_read LIBS tomlplusplus::tomlplusplus INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_content_index LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_livery_def LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_difficulty LIBS engine-difficulty INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_input LIBS engine-input)

fl_add_test(test_localization LIBS engine-i18n)

# Session-failure i18n coverage (#358): keys present in the repo locale/en/ui.toml + tr fallback.
# Longhand for the compile definition; the discover call stays with its own target (#1265).
add_executable(test_session_i18n test_session_i18n.cpp)
target_link_libraries(test_session_i18n PRIVATE Catch2::Catch2WithMain engine-i18n platform-stdfs platform-hal)
target_include_directories(test_session_i18n PRIVATE "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
target_compile_definitions(test_session_i18n PRIVATE FL_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(test_session_i18n)

fl_add_test(test_firstrun LIBS engine-firstrun INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_file_logger LIBS platform-file-logger)

fl_add_test(test_crash_reporter LIBS engine-crash platform-file-logger INCLUDES ${CMAKE_BINARY_DIR}/engine)

fl_add_test(test_userconfig LIBS engine-config INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_userconfig_enums LIBS engine-config INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_settings LIBS engine-config INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Missions-as-integration-tests harness (#856): run the ci-mission-pack smoke mission headless via
# fl-server --mission-report and assert on its JSON outcome. Gated on fl-server (absent in the
# lean/tools-only builds) and a Python 3 interpreter.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(TARGET fl-server AND Python3_Interpreter_FOUND)
    add_test(NAME mission_harness_ci_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/mission_test/mission_test.py"
                --server $<TARGET_FILE:fl-server>
                --assets "${CMAKE_SOURCE_DIR}/tests/fixtures/mission_root"
                --mission ci_smoke
                --expect-outcome success
                --min-triggers 2
                --min-spawned 2
    )
    # ci_smoke uses only builtin:debug-entity, so it never resolves a PACK flight model — the path
    # every real content pack hits on its first aircraft spawn, and the one a null cache crashed in
    # v0.3.16 (#1178). This leg spawns the fixture pack's one flight-model-bearing entity; the
    # survivor floor asserts the aircraft is still flying at mission end, not just that we didn't
    # crash getting it into the air.
    # A mission-attached Lua script reaching the live ATC service (#1288). Before the
    # construction/wiring split the mission path was handed a null AtcService, so atc.scramble was a
    # silent no-op and this run ended with ONE aircraft. The survivor floor of 2 is the whole test:
    # the second aircraft exists only because ATC actually launched a departure.
    add_test(NAME mission_harness_atc_scramble
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/mission_test/mission_test.py"
                --server $<TARGET_FILE:fl-server>
                --assets "${CMAKE_SOURCE_DIR}/tests/fixtures/mission_root"
                --mission ci_atc_scramble
                --expect-outcome success
                --min-spawned 1
                --min-survivors 2
    )

    add_test(NAME mission_harness_pack_flight_model
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/mission_test/mission_test.py"
                --server $<TARGET_FILE:fl-server>
                --assets "${CMAKE_SOURCE_DIR}/tests/fixtures/mission_root"
                --mission ci_flight_model
                --expect-outcome success
                --min-triggers 1
                --min-spawned 1
                --min-survivors 1
    )
endif()

# Integration: verify locale files are in sync with source key references
if(TARGET locale-extract)
    add_test(NAME locale_lint
        COMMAND $<TARGET_FILE:locale-extract>
                --src "${CMAKE_SOURCE_DIR}/engine"
                --locale "${CMAKE_SOURCE_DIR}/locale"
                --dry-run
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

if(TARGET platform-enet)
    add_executable(test_network test_network.cpp)
    target_link_libraries(test_network PRIVATE Catch2::Catch2WithMain platform-enet)
    target_include_directories(test_network PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
    )
    catch_discover_tests(test_network)
endif()

# Backend-selecting factory (enet path always available via the facade).
if(TARGET platform-net)
    add_executable(test_network_factory test_network_factory.cpp)
    target_link_libraries(test_network_factory PRIVATE Catch2::Catch2WithMain platform-net)
    target_include_directories(test_network_factory PRIVATE "${CMAKE_SOURCE_DIR}/platform/net")
    catch_discover_tests(test_network_factory)
endif()

# GameNetworkingSockets backend — only when FL_ENABLE_GNS built it.
if(TARGET platform-gns)
    add_executable(test_gns_network test_gns_network.cpp)
    target_link_libraries(test_gns_network PRIVATE Catch2::Catch2WithMain platform-gns)
    target_include_directories(test_gns_network PRIVATE "${CMAKE_SOURCE_DIR}/platform/net")
    catch_discover_tests(test_gns_network)
endif()

if(TARGET platform-enet)

    add_executable(test_server_config test_server_config.cpp)
    target_link_libraries(test_server_config PRIVATE Catch2::Catch2WithMain fl-server-config)
    target_include_directories(test_server_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_server_config)

    # The per-key reload-class table (#1081). Links the server library because the table's hot rows
    # carry the code that applies them.
    add_executable(test_config_reload test_config_reload.cpp)
    target_link_libraries(test_config_reload PRIVATE Catch2::Catch2WithMain fl-server-lib)
    target_include_directories(test_config_reload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_config_reload)

    # Pure geometry for the #573 load-test spawn spread (header-only, no link deps).
    add_executable(test_load_spawn test_load_spawn.cpp)
    target_link_libraries(test_load_spawn PRIVATE Catch2::Catch2WithMain)
    target_include_directories(test_load_spawn PRIVATE "${CMAKE_SOURCE_DIR}/server/fl-server")
    catch_discover_tests(test_load_spawn)

    # bot_swarm pure-logic tests (patterns, NetStats, CLI, metrics, server_tick block) — no
    # sockets. platform-hal supplies the IClock.h include pulled in via perf/ServerTickReport.h.
    add_executable(test_bot_swarm test_bot_swarm.cpp)
    target_link_libraries(test_bot_swarm PRIVATE Catch2::Catch2WithMain platform-hal)
    target_include_directories(test_bot_swarm PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/bot_swarm"
        "${CMAKE_SOURCE_DIR}/tools/common"
        "${CMAKE_SOURCE_DIR}/engine"
    )
    catch_discover_tests(test_bot_swarm)

    # Per-phase tick profiler + server tick report (header-only; platform-hal for IClock.h).
    add_executable(test_tick_profiler test_tick_profiler.cpp)
    target_link_libraries(test_tick_profiler PRIVATE Catch2::Catch2WithMain platform-hal)
    # platform/net for WireRateSampler.h (#772) — header-only, pure logic, no backend link needed.
    target_include_directories(test_tick_profiler PRIVATE
        "${CMAKE_SOURCE_DIR}/engine"
        "${CMAKE_SOURCE_DIR}/platform")
    catch_discover_tests(test_tick_profiler)

    add_executable(test_server_tick_report test_server_tick_report.cpp)
    target_link_libraries(test_server_tick_report PRIVATE Catch2::Catch2WithMain platform-hal)
    target_include_directories(test_server_tick_report PRIVATE "${CMAKE_SOURCE_DIR}/engine")
    catch_discover_tests(test_server_tick_report)

    # Client-side render-perf artifact behind the #782 GPU-contention harness. platform-hal brings
    # RenderTypes.h (FrameStats) and glm; the recorder itself is header-only.
    add_executable(test_frame_stats_recorder test_frame_stats_recorder.cpp)
    target_link_libraries(test_frame_stats_recorder PRIVATE Catch2::Catch2WithMain platform-hal)
    target_include_directories(test_frame_stats_recorder PRIVATE "${CMAKE_SOURCE_DIR}/engine")
    catch_discover_tests(test_frame_stats_recorder)

    add_executable(test_lan_discovery test_lan_discovery.cpp)
    target_link_libraries(test_lan_discovery PRIVATE Catch2::Catch2WithMain engine-net)
    target_include_directories(test_lan_discovery PRIVATE
        "${CMAKE_SOURCE_DIR}/engine"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_lan_discovery)

    add_executable(test_server_query test_server_query.cpp)
    target_link_libraries(test_server_query PRIVATE Catch2::Catch2WithMain engine-net)
    target_include_directories(test_server_query PRIVATE
        "${CMAKE_SOURCE_DIR}/engine"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_server_query)

    # The one AI-controller construction ladder, and the ATC-service parity it fixed (#1236).
    add_executable(test_ai_controller_build test_ai_controller_build.cpp)
    target_link_libraries(test_ai_controller_build PRIVATE
        Catch2::Catch2WithMain
        engine-ai engine-atc engine-entity engine-script engine-world fl-server-lib)
    target_include_directories(test_ai_controller_build PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_ai_controller_build)

    add_executable(test_admin_console test_admin_console.cpp)
    target_link_libraries(test_admin_console PRIVATE
        Catch2::Catch2WithMain
        fl-server-config engine-console engine-net engine-entity engine-weather engine-loop
        engine-ai engine-atc engine-world engine-script fl-server-lib)
    target_include_directories(test_admin_console PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_admin_console)

    # Server content bootstrap (#683): pack entity defs -> EntityTypeRegistry. Compiles the
    # server-layer helper directly (idiom from test_admin_console); mock_content.h from tests/.
    add_executable(test_content_bootstrap test_content_bootstrap.cpp)
    target_link_libraries(test_content_bootstrap PRIVATE
        Catch2::Catch2WithMain engine-bootstrap engine-content engine-entity engine-sensor engine-weapon)
    target_include_directories(test_content_bootstrap PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_content_bootstrap)

    # REST admin API (#233). Every security decision — token table, bearer extraction, constant-time
    # compare, issuer construction, untrusted body scanning — lives in free functions, so this covers
    # them without opening a socket (the test_rcon_server recipe).
    add_executable(test_http_admin test_http_admin.cpp)
    target_link_libraries(test_http_admin PRIVATE
        Catch2::Catch2WithMain fl-server-config engine-console engine-net engine-mission httplib::httplib fl-server-lib)
    target_include_directories(test_http_admin PRIVATE
    "${CMAKE_BINARY_DIR}/engine"  # generated Version.h, via McpEndpoint's initialize response
    "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(test_http_admin PRIVATE ws2_32)
    endif()
    catch_discover_tests(test_http_admin)

    # Deterministic voice phrase matching (#935). No model, no audio device — the matcher is pure.
    add_executable(test_wingman_phrase_match test_wingman_phrase_match.cpp)
    target_link_libraries(test_wingman_phrase_match PRIVATE Catch2::Catch2WithMain engine-ai)
    catch_discover_tests(test_wingman_phrase_match)

    # Chat-to-intent bridge (#611). Pure logic, canned responses, no model — the rejection and
    # injection paths are the point.
    add_executable(test_chat_intent_bridge test_chat_intent_bridge.cpp)
    target_link_libraries(test_chat_intent_bridge PRIVATE Catch2::Catch2WithMain engine-ai)
    catch_discover_tests(test_chat_intent_bridge)

    # AI provider seam (#163): the NullAiProvider contract and the WorldEvolutionDelta application
    # path, which is the one that has to be hostile to model output. No plugin on disk is needed —
    # the load-failure case uses a path that does not exist.
    add_executable(test_world_ai_provider test_world_ai_provider.cpp)
    target_link_libraries(test_world_ai_provider PRIVATE Catch2::Catch2WithMain engine-worldai)
    target_include_directories(test_world_ai_provider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_world_ai_provider)

    # MCP surface (#601). McpEndpoint is transport-free by design, so this drives every method, tier
    # refusal and session path without binding a port — and it links no httplib at all. engine-mission
    # arrives for submit_mission's in-process validateMission. SDL/ENet-free, so it joins the TSan set.
    add_executable(test_mcp_protocol test_mcp_protocol.cpp)
    target_link_libraries(test_mcp_protocol PRIVATE
        Catch2::Catch2WithMain fl-server-config engine-console engine-net engine-mission fl-server-lib)
    target_include_directories(test_mcp_protocol PRIVATE
    "${CMAKE_BINARY_DIR}/engine"  # generated Version.h — reported in initialize's serverInfo
    "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_mcp_protocol)

    add_executable(test_rcon_server test_rcon_server.cpp)
    target_link_libraries(test_rcon_server PRIVATE
        Catch2::Catch2WithMain fl-server-config engine-console fl-server-lib)
    target_include_directories(test_rcon_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(test_rcon_server PRIVATE ws2_32)
    endif()
    catch_discover_tests(test_rcon_server)

    # Admin-console stdin reader (#1038). Pure line assembly + the reader lifecycle; no fd, no
    # server. Threads is here for the platform read loop; everything else arrives with fl-server-lib.
    find_package(Threads REQUIRED)
    add_executable(test_stdin_reader test_stdin_reader.cpp)
    target_link_libraries(test_stdin_reader PRIVATE Catch2::Catch2WithMain Threads::Threads fl-server-lib)
    target_include_directories(test_stdin_reader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(test_stdin_reader)
endif()

# Shutdown-deadlock regression (#1038): spawns the REAL fl-server with its stdin held open (the
# condition the deadlock needs, and the way the game client starts single-player) and requires it to
# exit on `quit` within a deadline. Needs the built binary, so it is gated on the target; the path
# is passed WITHOUT an extension because Subprocess::spawn appends .exe on Windows itself.
if(TARGET fl-server AND TARGET platform-subprocess)
    add_executable(test_server_shutdown test_server_shutdown.cpp)
    target_link_libraries(test_server_shutdown PRIVATE Catch2::Catch2WithMain platform-subprocess platform-hal)
    target_compile_definitions(test_server_shutdown PRIVATE
        FL_SERVER_BIN="$<TARGET_FILE_DIR:fl-server>/fl-server")
    target_include_directories(test_server_shutdown PRIVATE
        "${CMAKE_SOURCE_DIR}/platform"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(test_server_shutdown fl-server)
    catch_discover_tests(test_server_shutdown)

    # Record<->replay fidelity gate (#644): run the real server with recording on, then require the
    # hashes recomputed from the DECODED file to equal the recorder's own. The sim-determinism half
    # lives in test_world_broadcaster ([determinism]) -- in process, because two networked runs are
    # not tick-aligned and comparing them would be flaky rather than strict.
    add_executable(test_replay_roundtrip test_replay_roundtrip.cpp)
    target_link_libraries(test_replay_roundtrip PRIVATE
        Catch2::Catch2WithMain engine-replay platform-subprocess platform-hal)
    target_compile_definitions(test_replay_roundtrip PRIVATE
        FL_SERVER_BIN="$<TARGET_FILE_DIR:fl-server>/fl-server")
    target_include_directories(test_replay_roundtrip PRIVATE
        "${CMAKE_SOURCE_DIR}/engine"
        "${CMAKE_SOURCE_DIR}/platform"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(test_replay_roundtrip fl-server)
    catch_discover_tests(test_replay_roundtrip)
endif()

# `.flrep` replay format (#643): writer/reader round-trip, seek, trailer rebuild, version rules and
# the hostile-input refusals. engine-replay reaches only engine-protocol + engine-compress, so this
# links no server sim and no platform backend.
add_executable(test_replay_format test_replay_format.cpp)
target_link_libraries(test_replay_format PRIVATE Catch2::Catch2WithMain engine-replay)
target_include_directories(test_replay_format PRIVATE
    "${CMAKE_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_replay_format)

add_executable(test_replay_reader_sections test_replay_reader_sections.cpp)
target_link_libraries(test_replay_reader_sections PRIVATE Catch2::Catch2WithMain engine-replay)
target_include_directories(test_replay_reader_sections PRIVATE
    "${CMAKE_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_replay_reader_sections)

# ReplayPlayer (#41): playback timing, speed control and scrubbing. game-client carries the player;
# engine-replay is the reader behind it.
add_executable(test_replay_player test_replay_player.cpp)
target_link_libraries(test_replay_player PRIVATE Catch2::Catch2WithMain engine-replay engine-render platform-hal game-client)
target_include_directories(test_replay_player PRIVATE
    "${CMAKE_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_replay_player)

# The replay browser (#41): the directory scan and the list/confirm behaviour, including the refusal
# path for a file this build cannot read.
add_executable(test_replay_select test_replay_select.cpp)
target_link_libraries(test_replay_select PRIVATE Catch2::Catch2WithMain engine-replay platform-hal game-client)
target_include_directories(test_replay_select PRIVATE
    "${CMAKE_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_replay_select)

# Mission-source resolution (server/fl-server/MissionSource.h, header-only): builtin id -> .yaml
# file path -> pack Mission asset. Needs engine-content for AssetManager only; no networking.
add_executable(test_mission_source test_mission_source.cpp)
target_link_libraries(test_mission_source PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_mission_source PRIVATE
    "${CMAKE_SOURCE_DIR}/server/fl-server"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_mission_source)

fl_add_test(test_ai_controllers LIBS engine-ai engine-entity engine-spatial engine-sensor)

fl_add_test(test_state_machine_controller LIBS engine-ai engine-entity engine-spatial engine-sensor)

# Takeoff / landing ground-and-pattern controllers (#701, ATC epic #673).
fl_add_test(test_takeoff_controller LIBS engine-ai engine-entity engine-spatial)

fl_add_test(test_landing_controller LIBS engine-ai engine-entity engine-spatial)

# ATC service core — facilities, runway sequencing, clearances (#702, ATC epic #673).
fl_add_test(test_atc_service LIBS engine-atc engine-world engine-entity engine-spatial)

fl_add_test(test_atc_facility_sequencing LIBS engine-atc engine-world engine-entity engine-spatial)

# The scripted wingman grammar + its two wire messages (#610). Header-only + engine-protocol only.
fl_add_test(test_wingman_command LIBS engine-protocol engine-world engine-ai)

# The formation / command tree (#610). Pure stdlib — no entity manager, no network.
fl_add_test(test_formation_registry LIBS engine-world)

# FormationController, boresight designation, and the six commands (#610).
fl_add_test(test_wingman_behavior LIBS engine-ai engine-entity engine-spatial)

fl_add_test(test_guidance LIBS engine-ai engine-entity engine-spatial)

fl_add_test(test_spatial_index LIBS engine-spatial)

# Isolated scale/characterisation benchmarks (issue #573). All cases are [.]-hidden so the default
# ctest run skips them; run explicitly via `ctest -R test_entity_scale` or the [scale] tag.
fl_add_test(test_entity_scale LIBS engine-entity engine-spatial INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_job_system LIBS engine-job)

# AdminChannel + AdminChannelRegistry (#1079): the six admin frontends' shared auth, dispatch and
# async-ack drain. engine-net only -- no sockets, no registry, injected clock, so the drain deadline is
# testable without opening a file descriptor (the test_rcon_server recipe generalised).
fl_add_test(test_admin_channel LIBS engine-net INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

# The engine's one JSON escaper, writer set and reader (#1080). Header-only and stdlib-only, so this
# links nothing: engine-protocol's zero-dep rule is the reason the helper is shaped that way.
# The one whole-file read over IFilesystem (#1254). Header-only over the HAL interface, so it needs
# no engine library -- the short-read case uses a local fake, since the shared mock never short-reads.
fl_add_test(test_fs_read INCLUDES "${CMAKE_SOURCE_DIR}/engine" "${CMAKE_SOURCE_DIR}/platform")

fl_add_test(test_json INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_faction_registry LIBS engine-world)

# Airspace alert system (#162): zone geometry, the escalation state machine (driven by an explicit
# simDt, so time is injectable without a clock), the escalation-policy TOML schema, and the
# sample-order independence the pass needs to stay deterministic.
fl_add_test(test_alert_system LIBS engine-world)

fl_add_test(test_game_mode LIBS engine-match)

fl_add_test(test_match_controller LIBS engine-match)

fl_add_test(test_airport_registry LIBS engine-world)

fl_add_test(test_airport_csv_import LIBS engine-world)

fl_add_test(test_airport_bootstrap LIBS engine-world INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_crew_partition LIBS engine-entity)

fl_add_test(test_airport_renderer LIBS engine-render engine-world)

fl_add_test(test_builtin_biomes LIBS engine-render)

fl_add_test(test_biome_weights LIBS engine-render)

add_executable(test_audio test_audio.cpp)
target_link_libraries(test_audio PRIVATE Catch2::Catch2WithMain platform-openal OpenAL::OpenAL)
target_include_directories(test_audio PRIVATE "${CMAKE_SOURCE_DIR}/platform/openal")
if(WIN32)
    add_custom_command(TARGET test_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL::OpenAL>
            $<TARGET_FILE_DIR:test_audio>
    )
endif()
catch_discover_tests(test_audio DISCOVERY_MODE PRE_TEST)

fl_add_test(test_subtitle_queue LIBS engine-audio)

fl_add_test(test_music_manager LIBS engine-audio engine-config)

fl_add_test(test_async_filesystem LIBS platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_sdl3 test_sdl3.cpp)
target_link_libraries(test_sdl3 PRIVATE Catch2::Catch2WithMain platform-sdl3 SDL3::SDL3)
target_include_directories(test_sdl3 PRIVATE "${CMAKE_SOURCE_DIR}/platform/sdl3")
if(WIN32)
    add_custom_command(TARGET test_sdl3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_sdl3>
    )
endif()
catch_discover_tests(test_sdl3 DISCOVERY_MODE PRE_TEST)

add_executable(test_cursor test_cursor.cpp)
target_link_libraries(test_cursor PRIVATE Catch2::Catch2WithMain platform-sdl3)
target_include_directories(test_cursor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/platform/sdl3"
)
if(WIN32)
    add_custom_command(TARGET test_cursor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_cursor>
    )
endif()
catch_discover_tests(test_cursor DISCOVERY_MODE PRE_TEST)

add_executable(test_display test_display.cpp)
target_link_libraries(test_display PRIVATE Catch2::Catch2WithMain platform-sdl3)
target_include_directories(test_display PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/platform/sdl3"
)
if(WIN32)
    add_custom_command(TARGET test_display POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_display>
    )
endif()
catch_discover_tests(test_display DISCOVERY_MODE PRE_TEST)

add_executable(test_joystick test_joystick.cpp)
target_link_libraries(test_joystick PRIVATE Catch2::Catch2WithMain platform-sdl3)
target_include_directories(test_joystick PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/platform/sdl3"
)
if(WIN32)
    add_custom_command(TARGET test_joystick POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_joystick>
    )
endif()
catch_discover_tests(test_joystick DISCOVERY_MODE PRE_TEST)

add_executable(test_input_hal test_input_hal.cpp)
target_link_libraries(test_input_hal PRIVATE Catch2::Catch2WithMain platform-sdl3)
target_include_directories(test_input_hal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/platform/sdl3"
)
if(WIN32)
    add_custom_command(TARGET test_input_hal POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_input_hal>
    )
endif()
catch_discover_tests(test_input_hal DISCOVERY_MODE PRE_TEST)

add_executable(test_validate_weapon test_validate_weapon.cpp)
# engine-weapon is PRIVATE to validate-weapon-lib, so link it here for BuiltinWeapon.h (#862).
target_link_libraries(test_validate_weapon PRIVATE Catch2::Catch2WithMain validate-weapon-lib engine-weapon)
catch_discover_tests(test_validate_weapon)

fl_add_test(test_validate_sensor LIBS validate-sensor-lib)

fl_add_test(test_validate_entity LIBS validate-entity-lib)

fl_add_test(test_validate_livery LIBS validate-livery-lib INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_validate_flight_model test_validate_flight_model.cpp)
target_link_libraries(test_validate_flight_model
    PRIVATE Catch2::Catch2WithMain validate-flight-model-lib)
catch_discover_tests(test_validate_flight_model)

add_executable(test_validate_mission test_validate_mission.cpp)
target_link_libraries(test_validate_mission
    PRIVATE Catch2::Catch2WithMain validate-mission-lib engine-mission) # engine-mission: parseMission for cameras: (#910)
catch_discover_tests(test_validate_mission)

# engine-mission runtime (#632): parser model population, parser/linter parity, and the sim-setup
# applier. Links engine-mission (parseMission/applyMission + EntityManager/WeatherController/
# FactionRegistry via its PUBLIC deps) and validate-mission-lib (the linter façade, for parity).
add_executable(test_mission_runtime test_mission_runtime.cpp)
target_link_libraries(test_mission_runtime
    PRIVATE Catch2::Catch2WithMain engine-mission validate-mission-lib)
catch_discover_tests(test_mission_runtime)

# ShotDirector camera-pose evaluator (#911): pure GLM+stdlib, no HAL — orbit/chase/move geometry,
# cut/gap/before-first/after-last handling, dead-target fallback, fov clamp.
add_executable(test_shot_director test_shot_director.cpp)
target_link_libraries(test_shot_director
    PRIVATE Catch2::Catch2WithMain engine-mission)
catch_discover_tests(test_shot_director)

# engine-campaign (#635): the deterministic dynamic-campaign runtime — frontline raster, campaign
# schema parser, and the theater-graph state machine (story injection, dynamic selection, save/restore).
# CampaignParser validation + referential integrity (#1145): every way a campaign can be malformed.
# The parser is the only thing between an authoring mistake and a campaign that loads broken.
# MissionParser schema validation (#1145): ~90 distinct rejections, written as perturbations of one
# valid mission because that is how authoring failures arrive — a working file with one thing wrong.
# FlightModelParser rejections (#1145): one entry point, five vehicle schemas, each with its own
# required tables. A model that loads with a table missing flies wrong instead of failing to load.
# AssetManager multi-pack resolution (#1145): which pack wins, what happens when one hands back
# rubbish, and what the cache does about it — the behaviour that makes modding work.
# createController (#1145): 23 behaviours behind one string, each with its own argument grammar.
# The surface an operator types at the console and an author writes in YAML — a malformed argument
# must yield null rather than a controller quietly flying on defaults nobody asked for.
# VoiceCalloutManager audio resolution (#1145): TTS vs packed OGG, the radio treatment, the buffer
# cache, and every way each of those can be unavailable. The subsystem is built to degrade; that is
# only true if it is exercised.
# The pack-loading half of ContentBootstrap (#1145): airports, zone policies and weapons share the
# entity-def loader's list/load/parse/register shape. One broken file in a downloaded pack must cost
# the player that one asset, never the pack — and must name the file so an author can find it.
# AircraftManual for the non-fixed-wing vehicles and the full store/sensor vocabularies (#1145).
# The enum-to-name tables are the kind of thing that silently grows a "?" when an enumerator is added.
# LobbyListClient's request lifecycle (#1145): URL construction, the in-flight guard, chunked body
# accumulation, and every way a fetch against a remote lobby can fail.
# EntityDefParser's rejection rules (#1145): hardpoints, turrets, crew seats, sensors, decks. Each
# rule exists because the alternative is an entity that registers and then misbehaves.
fl_add_test(test_entity_def_parser_rules LIBS engine-entity)

fl_add_test(test_lobby_list_client LIBS engine-net platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_aircraft_manual_vehicles LIBS engine-manual engine-flight engine-entity engine-weapon engine-sensor)

fl_add_test(test_content_bootstrap_loaders
    LIBS engine-bootstrap engine-content engine-entity engine-weapon engine-world
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_voice_callout_paths
    LIBS engine-audio engine-content engine-radio engine-i18n
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_ai_controller_factory LIBS engine-ai engine-entity engine-spatial engine-sensor engine-worldai)

fl_add_test(test_asset_manager_fallback LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_flight_model_parser_validation LIBS engine-flight)

fl_add_test(test_mission_parser_validation LIBS engine-mission)

fl_add_test(test_campaign_parser_validation LIBS engine-campaign)

add_executable(test_campaign test_campaign.cpp)
target_link_libraries(test_campaign
    PRIVATE Catch2::Catch2WithMain engine-campaign engine-mission) # engine-mission: a generated sortie must parse
catch_discover_tests(test_campaign)

fl_add_test(test_campaign_engine_branches LIBS engine-campaign)

add_executable(test_validate_licenses test_validate_licenses.cpp)
target_link_libraries(test_validate_licenses
    PRIVATE Catch2::Catch2WithMain validate-licenses-lib)
catch_discover_tests(test_validate_licenses)

add_executable(test_validate_mesh test_validate_mesh.cpp)
target_link_libraries(test_validate_mesh
    PRIVATE Catch2::Catch2WithMain validate-mesh-lib)
catch_discover_tests(test_validate_mesh)

# validate-campaign (#847) + the frontline PNG codec.
fl_add_test(test_frontline_png LIBS engine-campaign-png)

# The one untrusted-PNG chunk guard behind terrain, frontline and the Vulkan texture loader (#1237).
fl_add_test(test_png_sanity LIBS platform-hal)

fl_add_test(test_validate_campaign LIBS validate-campaign-lib engine-campaign-png INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# validate-mod (#651): the whole-pack validator.
fl_add_test(test_validate_mod LIBS validate-mod-lib engine-crypto INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Mesh node-tree description for the fl-viewer panel (#838).
fl_add_test(test_mesh_node_tree LIBS validate-mesh-lib)

add_executable(test_tex_compress test_tex_compress.cpp)
target_link_libraries(test_tex_compress
    PRIVATE Catch2::Catch2WithMain tex-compress-lib)
catch_discover_tests(test_tex_compress)

# Real std::filesystem async backend, exercised against a temp dir. No runtime
# DLL (unlike the former SDL3 backend), so default discovery works.
add_executable(test_async_filesystem_std test_async_filesystem_std.cpp)
target_link_libraries(test_async_filesystem_std PRIVATE Catch2::Catch2WithMain platform-stdfs)
target_include_directories(test_async_filesystem_std PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/platform/stdfs"
)
catch_discover_tests(test_async_filesystem_std)

# Pure wire-protocol/codec tests link the zero-dep engine-protocol seam, not engine-net.
fl_add_test(test_game_protocol LIBS engine-protocol engine-weather)

fl_add_test(test_wire_codec LIBS engine-protocol)

fl_add_test(test_snapshot_codec LIBS engine-protocol)

# Scheduler lives in engine-net (SnapshotScheduler.cpp), so this test keeps the engine-net link.
fl_add_test(test_snapshot_scheduler LIBS engine-net)

fl_add_test(test_ack_window LIBS engine-protocol)

# --- Epic J: in-game voice comms (#499) --------------------------------------------------------
# Opus codec wrapper (#531). Real libopus round trips, so a broken encoder configuration or a
# regressed payload cap fails here rather than on a live server.
fl_add_test(test_voice_codec LIBS engine-voice)

# Per-speaker voice jitter buffer (#531; pure, no codec/device).
fl_add_test(test_voice_jitter LIBS engine-voice)

# PTT/VOX keying gate (#531; pure, header-only).
fl_add_test(test_voice_activity LIBS engine-voice)

# Radio-net table + the builtin zero-config net stack (#531/#532).
fl_add_test(test_radio_net LIBS engine-voice)

# Remote voice playback / positional mix (#531) against a queue-tracking IAudio double.
fl_add_test(test_voice_mixer LIBS engine-voice)

# The client voice facade (#531). Its five components each had a test; the object that decides WHEN
# to key, WHICH net to key on, and when the capture device runs had none — 0.0% branch coverage
# until #1145, and that is exactly where the player-visible behaviour lives.
fl_add_test(test_voice_chat LIBS engine-voice INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# Radio presentation DSP (#925): band-limiting, soft clipping, and the byte-stable click/squelch cues.
fl_add_test(test_radio_dsp LIBS engine-radio)

# Voice routing (#532): who hears a transmission, as pure functions over a net table + peer list.
fl_add_test(test_voice_router LIBS engine-net)

# Server capability vocabulary + role presets (#945; header-only, stdlib).
fl_add_test(test_capability LIBS engine-protocol)

# Aggregated world-state surface (#600; pure builder, SDL/ENet-free).
fl_add_test(test_world_state LIBS engine-net)

# The match event log (#600): ring bounds + gap detection, JSON schema stability, string escaping,
# and a concurrent append/read case. Links only engine-net (no SDL/ENet), so it joins the TSan set
# alongside test_world_state -- the log is written on the sim thread and read from another.
fl_add_test(test_match_event_log LIBS engine-net)

# MissionReport stays valid JSON whatever the mission is named (#1234).
fl_add_test(test_mission_report LIBS engine-mission)

# Engine-layer snapshot payload codec (#775; zstd wrapper in engine-compress).
fl_add_test(test_snapshot_compression LIBS engine-compress)

# FLIT input-trace codec (writer + reader; header-only, stdlib-only — #560).
fl_add_test(test_input_trace LIBS engine-protocol)

fl_add_test(test_congestion_controller LIBS engine-net)

# The one tick-rate authority (#1075) and the conversions #1253 folded back into it. Header-only
# and stdlib-only, so no engine library is needed.
fl_add_test(test_tick_rate INCLUDES "${CMAKE_SOURCE_DIR}/engine")

fl_add_test(test_tick_governor LIBS engine-net)

fl_add_test(test_performance_overlay LIBS engine-perf)

fl_add_test(test_utf8_decode LIBS platform-hal)

# The no-op IAudio (#1117) — the degradation path a machine with no audio device takes, and the
# base every audio test double derives from. Header-only, so platform-hal is the whole dependency.
fl_add_test(test_null_audio LIBS platform-hal)

# IGui HAL contract test (#156) — drives the scripted NullGui (tests/mock_gui.h), the seam every
# UI-screen test uses. platform-hal supplies IGui.h; the real Dear ImGui backend needs a GPU (not in CI).
fl_add_test(test_gui LIBS platform-hal)

# JoinServerScreen (#322) — the first IGui screen, driven against NullGui + the HAL mocks.
fl_add_test(test_join_server_screen LIBS platform-hal game-client)

# ServerBrowserScreen (#143) — the IGui server-browser list, driven against NullGui.
fl_add_test(test_server_browser_screen LIBS engine-net platform-hal game-client)

# ScoreboardOverlay (#647) — the IGui scoreboard table, driven against NullGui.
fl_add_test(test_scoreboard_overlay LIBS platform-hal game-client)

# KillFeed (#647) — the HudElement kill-feed ring; header-only, deterministic clock.
fl_add_test(test_kill_feed LIBS platform-hal INCLUDES ${CMAKE_SOURCE_DIR}/game/fighters-legacy)

# ChatOverlay (#646) — the chat display ring + IGui input box, driven against NullGui + a manual clock.
fl_add_test(test_chat_overlay LIBS engine-net platform-hal game-client)

# Frame-capture swizzle helper (#912): the pure GPU-independent core of the capture readback.
fl_add_test(test_capture_frame LIBS platform-hal)

# Capture-boundary scheduler (#916): the pure tick-stream -> output-frame / dup-count logic behind the
# cinematic recorder. Header-only, no HAL — just needs the game include dir for RecordScheduler.h.
fl_add_test(test_record_scheduler INCLUDES "${CMAKE_SOURCE_DIR}/game/fighters-legacy")

fl_add_test(test_hud_align LIBS platform-hal)

# Content → body mesh orientation mapping (#906); header-only, glm via platform-hal.
fl_add_test(test_mesh_orient LIBS platform-hal)

fl_add_test(test_game_console LIBS engine-console engine-entity engine-weather INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_console_commands_paths LIBS engine-console engine-entity)

fl_add_test(test_command_shell LIBS engine-console INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_world_broadcaster
    LIBS engine-net engine-weather engine-bootstrap) # builtinDebugEntityDef/registerBuiltinWeapons — the sandbox arming smoke (#440)

# The admission half of the old broadcaster suite (#1085), sharing its fixtures via
# world_broadcaster_test_util.h. Same link set: it drives PeerAdmission through its owner.
fl_add_test(test_peer_admission LIBS engine-net engine-weather engine-bootstrap)

# The snapshot half (#1086), same arrangement.
fl_add_test(test_snapshot_pipeline LIBS engine-net engine-weather engine-bootstrap)

# The comms half (#1087), same arrangement.
fl_add_test(test_session_comms LIBS engine-net engine-weather engine-bootstrap)

add_executable(test_weather test_weather.cpp)
# engine-world for SandboxHome.h — the solar case contrasts the home with the pole it replaced (#1211).
target_link_libraries(test_weather PRIVATE Catch2::Catch2WithMain engine-weather engine-world)

fl_add_test(test_celestial LIBS engine-weather engine-flight)
catch_discover_tests(test_weather)

fl_add_test(test_solar_position LIBS engine-weather)

fl_add_test(test_magnetic_model LIBS engine-nav)

fl_add_test(test_terrain_chunk_io LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_terrain_streamer LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_terrain_mesh_builder LIBS engine-render INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_config_file LIBS engine-config)

# The shared ILogger doubles in mock_log.h (#1273): the query semantics ~15 suites now rely on,
# including the null-message guard the hand-rolled copies disagreed about.
fl_add_test(test_mock_log LIBS platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_network_utils LIBS engine-net)

fl_add_test(test_subprocess LIBS platform-subprocess)

fl_add_test(test_manifest_sanitization LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_trust_framework LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_mod_loader_load LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_asset_validator LIBS engine-content INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_lua_sandbox test_lua_sandbox.cpp)
# lua::lua is linked directly (engine-script keeps it PRIVATE) so this suite can
# register a probe C function and assert that a Lua error unwinds C++ frames —
# the property the whole build-Lua-as-C++ decision rests on (#1015).
# platform-stdfs: the require() tests read their modules through a REAL IFilesystem rooted at a temp
# assets dir, which is what makes the assets-domain resolution (#1210) testable at all.
target_link_libraries(test_lua_sandbox PRIVATE Catch2::Catch2WithMain engine-script lua::lua platform-stdfs)
target_include_directories(test_lua_sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_lua_sandbox)

add_executable(test_lua_controller test_lua_controller.cpp)
target_link_libraries(test_lua_controller PRIVATE
    Catch2::Catch2WithMain engine-script engine-spatial engine-sensor platform-stdfs
    engine-flight engine-ai) # ai_flight_harness.h: the closed-loop runs for builtin:fighter (#1221)
target_include_directories(test_lua_controller PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The doc-example tests LIFT their Lua out of ai.md at run time rather than holding a copy of it
# (#1180). A hand-transcribed "verbatim" copy is only verbatim until the next edit, and it drifted
# twice before this: #830 first, then #1147 corrected the test and left the guide broken.
target_compile_definitions(test_lua_controller PRIVATE
    FL_AI_MD_PATH="${CMAKE_SOURCE_DIR}/docs/modding/ai.md")
catch_discover_tests(test_lua_controller)

add_executable(test_haptic_controller test_haptic_controller.cpp)
target_link_libraries(test_haptic_controller PRIVATE
    Catch2::Catch2WithMain engine-render platform-hal game-client)
target_include_directories(test_haptic_controller PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_haptic_controller)

add_executable(test_precipitation_controller test_precipitation_controller.cpp)
target_link_libraries(test_precipitation_controller PRIVATE
    Catch2::Catch2WithMain engine-render)
target_include_directories(test_precipitation_controller PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_precipitation_controller)

add_executable(test_flight_input_collector test_flight_input_collector.cpp)
target_link_libraries(test_flight_input_collector PRIVATE
    Catch2::Catch2WithMain
    platform-hal
    platform-sdl3
    SDL3::SDL3
    engine-render
    engine-spatial
    engine-console
    engine-config
    engine-input
    engine-net game-client)
target_include_directories(test_flight_input_collector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    add_custom_command(TARGET test_flight_input_collector POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_flight_input_collector>)
endif()
catch_discover_tests(test_flight_input_collector)

add_executable(test_server_notice test_server_notice.cpp)
target_link_libraries(test_server_notice PRIVATE
    Catch2::Catch2WithMain platform-hal)
target_include_directories(test_server_notice PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy"
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_server_notice)

# ClientNetEventHandler dispatches MsgWingmanAck into the radio menu (#610), routes the effects TLV
# through ClientEffectRouter, and the router drives own-ship haptics (#631); game-client carries them all.
add_executable(test_client_net_event_handler test_client_net_event_handler.cpp)
target_link_libraries(test_client_net_event_handler PRIVATE
    Catch2::Catch2WithMain engine-net engine-render engine-entity engine-console engine-weather engine-audio
    engine-config platform-hal game-client)
target_include_directories(test_client_net_event_handler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_client_net_event_handler)

# Game-master overview map overlay (#861): drives the overlay against NullGui + a ClientNetEventHandler
# fed a GM feed. Same link set as the handler test plus the game IGui HAL header.
add_executable(test_gm_map_overlay test_gm_map_overlay.cpp)
target_link_libraries(test_gm_map_overlay PRIVATE
    Catch2::Catch2WithMain engine-net engine-render engine-entity engine-console engine-weather engine-audio
    engine-config platform-hal game-client)
target_include_directories(test_gm_map_overlay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_gm_map_overlay)

# ---------------------------------------------------------------------------
# Screen / ScreenManager tests
# ---------------------------------------------------------------------------

add_executable(test_main_menu_screen test_main_menu_screen.cpp)
target_link_libraries(test_main_menu_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
target_include_directories(test_main_menu_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_main_menu_screen)

add_executable(test_loading_screen test_loading_screen.cpp)
target_link_libraries(test_loading_screen PRIVATE Catch2::Catch2WithMain platform-hal engine-i18n game-client)
target_include_directories(test_loading_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_loading_screen)

add_executable(test_mission_select_screen test_mission_select_screen.cpp)
target_link_libraries(test_mission_select_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
target_include_directories(test_mission_select_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_mission_select_screen)

add_executable(test_mission_brief_screen test_mission_brief_screen.cpp)
target_link_libraries(test_mission_brief_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
target_include_directories(test_mission_brief_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_mission_brief_screen)

add_executable(test_settings_screen test_settings_screen.cpp)
target_link_libraries(test_settings_screen PRIVATE
    Catch2::Catch2WithMain engine-config platform-hal game-client)
target_include_directories(test_settings_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_settings_screen)

# The one menu input contract behind the six screens (#1260).
fl_add_test(test_menu_nav LIBS platform-hal game-client INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_pause_menu_screen test_pause_menu_screen.cpp)
target_link_libraries(test_pause_menu_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
target_include_directories(test_pause_menu_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_pause_menu_screen)

add_executable(test_debrief_screen test_debrief_screen.cpp)
target_link_libraries(test_debrief_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
target_include_directories(test_debrief_screen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_debrief_screen)

# The whole screen stack plus the overlays and controllers it drives, including the voice tier
# FlightScreen updates per frame (#935) -- all of it arrives with game-client.
add_executable(test_screen_manager test_screen_manager.cpp)
target_link_libraries(test_screen_manager PRIVATE Catch2::Catch2WithMain game-client)
target_include_directories(test_screen_manager PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_screen_manager)

add_executable(test_connect_args test_connect_args.cpp)
target_link_libraries(test_connect_args PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_connect_args PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy"
    "${CMAKE_SOURCE_DIR}/engine") # header-only: util/Parse.h for isAllDigits (#1244)
catch_discover_tests(test_connect_args)

# #1054: the embedded single-player server must not contend for the standard game port. Header-only
# use of LocalServer.h (a constexpr port) plus DiscoveryBeacon's default port -- engine-console and
# platform-hal are here for LocalServer.h's includes, not for any linked code.
add_executable(test_local_server_port test_local_server_port.cpp)
target_link_libraries(test_local_server_port PRIVATE
    Catch2::Catch2WithMain engine-net engine-console platform-hal)
target_include_directories(test_local_server_port PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_local_server_port)

# Observer entity picker (#860): pure logic over EntityRenderEntry; engine-render supplies the header +
# GLM include dirs (no SDL/rendering).
add_executable(test_entity_selector test_entity_selector.cpp)
target_link_libraries(test_entity_selector PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_entity_selector PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_entity_selector)

# Game-master map plan-view transform (#861; pure logic, glm-only).
add_executable(test_gm_map_view test_gm_map_view.cpp)
target_link_libraries(test_gm_map_view PRIVATE Catch2::Catch2WithMain engine-protocol platform-hal)
target_include_directories(test_gm_map_view PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_gm_map_view)

# The in-flight comms menu (#704, ATC epic #673): pure page-navigation/command-emission logic over
# IInput; compiles the game source directly, no SDL/rendering.
add_executable(test_comms_menu test_comms_menu.cpp)
target_link_libraries(test_comms_menu PRIVATE Catch2::Catch2WithMain engine-protocol platform-hal game-client)
target_include_directories(test_comms_menu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_comms_menu)

# Crew seat picker (#975): pure roster-driven seat selection (no SDL). engine-entity for CrewCapability,
# engine-protocol for SeatOccupancy (GameProtocol.h).
add_executable(test_crew_seat_menu test_crew_seat_menu.cpp)
target_link_libraries(test_crew_seat_menu PRIVATE Catch2::Catch2WithMain engine-entity engine-protocol)
target_include_directories(test_crew_seat_menu PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_crew_seat_menu)

# Client turret pose predictor (#979): the pure stepTurret-reuse predictor (no SDL). engine-weapon owns
# the shared servo (stepTurret/turretWorldDir).
add_executable(test_turret_predictor test_turret_predictor.cpp)
target_link_libraries(test_turret_predictor PRIVATE Catch2::Catch2WithMain engine-weapon)
target_include_directories(test_turret_predictor PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_turret_predictor)

# The one load-and-parse step behind both flight-model resolvers (#1232): malformed content is a
# null model plus a reason, never an exception into a spawn path.
add_executable(test_flight_model_load test_flight_model_load.cpp)
target_link_libraries(test_flight_model_load PRIVATE
    Catch2::Catch2WithMain engine-flight engine-content)
target_include_directories(test_flight_model_load PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_flight_model_load)

# The one GraphicsSettings -> RendererSettings mapping; pins the bloom-follows-preset rule (#1235).
add_executable(test_renderer_settings_map test_renderer_settings_map.cpp)
target_link_libraries(test_renderer_settings_map PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_renderer_settings_map PRIVATE
    "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_renderer_settings_map)

# The client/server flight-model parity gate (#811): drives the real WorldBroadcaster connect path
# and the real client-side resolver, and asserts the two integrate the same aeroplane.
add_executable(test_prediction_parity test_prediction_parity.cpp)
target_link_libraries(test_prediction_parity PRIVATE
    Catch2::Catch2WithMain engine-net engine-entity engine-content engine-flight engine-render engine-config
    engine-weapon game-client)
target_include_directories(test_prediction_parity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_prediction_parity)

add_executable(test_client_prediction test_client_prediction.cpp)
target_link_libraries(test_client_prediction PRIVATE
    Catch2::Catch2WithMain
    engine-flight
    engine-render
    engine-net
    engine-config game-client)
target_include_directories(test_client_prediction PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_client_prediction)

# ClientEffectRouter (#625/#631): wire effect vocabulary -> particles + SFX + haptics.
add_executable(test_effect_router test_effect_router.cpp)
target_link_libraries(test_effect_router PRIVATE
    Catch2::Catch2WithMain
    engine-render
    engine-protocol
    engine-audio
    engine-config
    platform-hal game-client)
catch_discover_tests(test_effect_router)

# SfxBuiltinSounds byte-stability + SfxManager voice pool / camera-relative play (#631).
fl_add_test(test_sfx LIBS engine-audio engine-config engine-content INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

fl_add_test(test_warning_tones LIBS engine-audio engine-config engine-content INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

# #959 — engine/aero doppler layers: pure mapping + byte-stable loops (engine-audio) plus the
# game-layer EngineAudioManager source wiring, compiled in directly.
add_executable(test_engine_audio test_engine_audio.cpp)
target_link_libraries(test_engine_audio PRIVATE Catch2::Catch2WithMain engine-audio engine-config engine-content game-client)
target_include_directories(test_engine_audio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_engine_audio)

fl_add_test(test_sha256 LIBS engine-crypto)

# The full-entropy token generator behind the admin token, MCP session ids and the pilot GUID (#1233).
fl_add_test(test_random_token LIBS engine-crypto)

add_executable(test_content_downloader test_content_downloader.cpp)
# engine-net for LobbyListClient: the #1083 regression drives TWO IHttpClient consumers at once,
# which is the case the old single-slot handler could not serve.
target_link_libraries(test_content_downloader PRIVATE Catch2::Catch2WithMain engine-content engine-crypto
    engine-net)
target_include_directories(test_content_downloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Lobby / server browser (#143): pure parser, registration client, and merge model.
fl_add_test(test_lobby_list_parser LIBS engine-net platform-hal)

fl_add_test(test_lobby_registration LIBS engine-net platform-hal INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

fl_add_test(test_server_browser_model LIBS engine-net platform-hal)
catch_discover_tests(test_content_downloader)

fl_add_test(test_wind_profile LIBS engine-weather engine-flight)
