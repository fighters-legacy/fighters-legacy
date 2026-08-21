# Test suite — Catch2 via cmake/dependencies.cmake
include(CTest)
include(Catch)

# Tests are registered here as engine code lands:
#   add_executable(test_foo foo_test.cpp)
#   target_link_libraries(test_foo PRIVATE Catch2::Catch2WithMain engine-content)
#   catch_discover_tests(test_foo)

add_executable(test_renderer test_renderer.cpp)
target_link_libraries(test_renderer PRIVATE Catch2::Catch2WithMain platform-hal)
target_include_directories(test_renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_renderer)

# MeshNodePlan (#839/#882): the glTF scene-graph walk + variant node-set filter + articulation-pose
# composition behind node-aware mesh loading. GPU-free by construction, so it runs in every CI leg.
add_executable(test_mesh_node_plan test_mesh_node_plan.cpp)
target_link_libraries(test_mesh_node_plan PRIVATE Catch2::Catch2WithMain platform-meshgraph)
target_include_directories(test_mesh_node_plan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_mesh_node_plan)

# MeshArticulation (#840): the channel registry, clip parser and scrub sampler. Engine-side and
# GPU-free -- the rig is parsed from the same .glb bytes the renderer uploads.
add_executable(test_mesh_articulation test_mesh_articulation.cpp)
target_link_libraries(test_mesh_articulation PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_mesh_articulation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_mesh_articulation)

add_executable(test_mesh_articulation_paths test_mesh_articulation_paths.cpp)
target_link_libraries(test_mesh_articulation_paths PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_mesh_articulation_paths PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_mesh_articulation_paths)

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE Catch2::Catch2WithMain platform-hal)
target_include_directories(test_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_window)

add_executable(test_bridge test_bridge.cpp)
target_link_libraries(test_bridge PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_bridge)

add_executable(test_scene_renderer test_scene_renderer.cpp)
target_link_libraries(test_scene_renderer PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_scene_renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_scene_renderer)

# StdFilesystemWatcher (#152): the polling hot-reload backend.
add_executable(test_std_filesystem_watcher test_std_filesystem_watcher.cpp)
target_link_libraries(test_std_filesystem_watcher PRIVATE Catch2::Catch2WithMain platform-stdfs)
target_include_directories(test_std_filesystem_watcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_std_filesystem_watcher)

# PreviewScene (#666): the authoring-tools single-model preview bootstrap. engine-content is a PRIVATE
# dep of engine-render, so link it explicitly for AssetManager in the test.
add_executable(test_preview_scene test_preview_scene.cpp)
target_link_libraries(test_preview_scene PRIVATE Catch2::Catch2WithMain engine-render engine-content)
target_include_directories(test_preview_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_preview_scene)

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

add_executable(test_flight_hud test_flight_hud.cpp)
target_link_libraries(test_flight_hud PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_flight_hud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_flight_hud)

add_executable(test_windshield_rain test_windshield_rain.cpp)
target_link_libraries(test_windshield_rain PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_windshield_rain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_windshield_rain)

add_executable(test_particle_system test_particle_system.cpp)
target_link_libraries(test_particle_system PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_particle_system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_particle_system)

add_executable(test_entity test_entity.cpp)
target_link_libraries(test_entity PRIVATE Catch2::Catch2WithMain engine-entity engine-spatial)
target_include_directories(test_entity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_entity)

# Terrain line-of-sight segment query (#687): header-only, pure engine-spatial (no glm).
add_executable(test_los test_los.cpp)
target_link_libraries(test_los PRIVATE Catch2::Catch2WithMain engine-spatial)
target_include_directories(test_los PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_los)

# World-to-HUD projection helper (#692): pure math, checked against CameraController::view().
add_executable(test_hud_projection test_hud_projection.cpp)
target_link_libraries(test_hud_projection PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_hud_projection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_hud_projection)

# Player autopilot (#640): pure hold-controller + closed-loop convergence through the integrator.
add_executable(test_autopilot test_autopilot.cpp)
target_link_libraries(test_autopilot PRIVATE Catch2::Catch2WithMain engine-flight engine-ai game-client)
target_include_directories(test_autopilot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_autopilot)

# Turn-law audit (#1143): every controller that commands a roll, flown through the real integrator
# in a scenario that keeps its heading error alive. Station-keeping controllers must stay upright;
# manoeuvre controllers must still be able to roll hard, pinned so nobody "consistency"-fixes them.
add_executable(test_ai_turn_law test_ai_turn_law.cpp)
target_link_libraries(test_ai_turn_law PRIVATE Catch2::Catch2WithMain engine-flight engine-ai engine-entity
    engine-spatial engine-sensor)
catch_discover_tests(test_ai_turn_law)

# AI altitude hold (#1141): a LoiterController flown through the real integrator. Nothing asserted
# that an AI aircraft asked to hold an altitude actually does, which is how a several-hundred-metre
# oscillation went unnoticed until it started flying the load-spawn world into the ground.
# LSO + crew-chief voice keys (#1108): a pack records OGGs against these names, so the strings are
# pinned rather than incidental.
add_executable(test_crew_phrases test_crew_phrases.cpp)
target_link_libraries(test_crew_phrases PRIVATE Catch2::Catch2WithMain engine-atc)
catch_discover_tests(test_crew_phrases)

add_executable(test_ai_altitude_hold test_ai_altitude_hold.cpp)
target_link_libraries(test_ai_altitude_hold PRIVATE Catch2::Catch2WithMain engine-flight engine-ai engine-entity
    engine-spatial engine-sensor)
target_include_directories(test_ai_altitude_hold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_ai_altitude_hold)

# Client-side target designation (#696): pure snapshot cycling / filtering / best-in-cone.
add_executable(test_target_designation test_target_designation.cpp)
target_link_libraries(test_target_designation PRIVATE Catch2::Catch2WithMain engine-render engine-entity game-client)
target_include_directories(test_target_designation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_target_designation)

# Padlock tracker (#697): pure aim + lock state machine, scripted trajectories.
# Wingman brevity voice keys (#1108). Links game-client: the wingman's text and keys are CLIENT-side
# by policy (the server sends a result code, never a string).
add_executable(test_wingman_brevity test_wingman_brevity.cpp)
target_link_libraries(test_wingman_brevity PRIVATE Catch2::Catch2WithMain engine-render engine-entity game-client)
target_include_directories(test_wingman_brevity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_wingman_brevity)

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
add_executable(test_ejection test_ejection.cpp)
target_link_libraries(test_ejection PRIVATE Catch2::Catch2WithMain engine-entity)
catch_discover_tests(test_ejection)

add_executable(test_atmosphere test_atmosphere.cpp)
target_link_libraries(test_atmosphere PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_atmosphere)

add_executable(test_table2d test_table2d.cpp)
target_link_libraries(test_table2d PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_table2d)

add_executable(test_flight_model_parser test_flight_model_parser.cpp)
target_link_libraries(test_flight_model_parser PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_flight_model_parser)

add_executable(test_weapon_def_parser test_weapon_def_parser.cpp)
target_link_libraries(test_weapon_def_parser PRIVATE Catch2::Catch2WithMain engine-weapon)
catch_discover_tests(test_weapon_def_parser)

# TransformHistory (#425): the lag-compensation rewind ring (header-only, engine-net).
# Entity-entity collision geometry + damage helpers (#630).
add_executable(test_collision test_collision.cpp)
target_link_libraries(test_collision PRIVATE Catch2::Catch2WithMain engine-entity)
catch_discover_tests(test_collision)

# Per-subsystem damage routing (#675) — the pure pick/apply logic.
add_executable(test_subsystem_damage test_subsystem_damage.cpp)
target_link_libraries(test_subsystem_damage PRIVATE Catch2::Catch2WithMain engine-entity)
catch_discover_tests(test_subsystem_damage)

add_executable(test_transform_history test_transform_history.cpp)
target_link_libraries(test_transform_history PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_transform_history)

# Required-pack policy (#872): the pure spec/policy parsing + missing-pack comparison (header-only).
add_executable(test_required_pack_policy test_required_pack_policy.cpp)
target_link_libraries(test_required_pack_policy PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_required_pack_policy)

# FireControl + ProjectileSystem (#625): the pure fire-decision logic and the 3-DOF projectile pool.
# engine-weapon's entity/spatial deps are PRIVATE, so the test links them explicitly.
add_executable(test_fire_control test_fire_control.cpp)
target_link_libraries(test_fire_control PRIVATE Catch2::Catch2WithMain engine-weapon engine-entity
    engine-spatial)
catch_discover_tests(test_fire_control)

# Turret mounts (#970): the pure slew-servo + pose math (stepTurret, turretWorldDir).
add_executable(test_turret test_turret.cpp)
target_link_libraries(test_turret PRIVATE Catch2::Catch2WithMain engine-weapon)
catch_discover_tests(test_turret)

# Crewed control frame (#969): per-seat sampling + turret slew + per-seat fire through a real WB tick.
add_executable(test_crew_frame test_crew_frame.cpp)
target_link_libraries(test_crew_frame PRIVATE Catch2::Catch2WithMain engine-net engine-entity engine-weapon
    engine-spatial engine-weather engine-job)
target_include_directories(test_crew_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_crew_frame)

# Bot turret gunner + per-instance skill (#971): honest-sensing gunnery through a real WB tick.
add_executable(test_turret_gunner test_turret_gunner.cpp)
target_link_libraries(test_turret_gunner PRIVATE Catch2::Catch2WithMain engine-net engine-ai engine-sensor
    engine-entity engine-weapon engine-spatial engine-weather engine-job)
target_include_directories(test_turret_gunner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_turret_gunner)

# SeekerTrack + PN guidance (#627): the missile seeker over the shared Detection.h machine.
add_executable(test_seeker test_seeker.cpp)
target_link_libraries(test_seeker PRIVATE Catch2::Catch2WithMain engine-weapon engine-entity
    engine-sensor engine-spatial)
catch_discover_tests(test_seeker)

# Chaff/flare countermeasure decoys + seeker seduction (#529).
add_executable(test_countermeasure_system test_countermeasure_system.cpp)
target_link_libraries(test_countermeasure_system PRIVATE Catch2::Catch2WithMain engine-weapon
    engine-entity engine-sensor engine-spatial)
catch_discover_tests(test_countermeasure_system)

# Bombs/rockets release ballistics + CCIP (#629).
add_executable(test_release_ballistics test_release_ballistics.cpp)
target_link_libraries(test_release_ballistics PRIVATE Catch2::Catch2WithMain engine-weapon
    engine-entity engine-spatial)
catch_discover_tests(test_release_ballistics)

# Ballistic missile guidance + the MIRV controller-spawn seam (#355).
add_executable(test_ballistic_guidance test_ballistic_guidance.cpp)
target_link_libraries(test_ballistic_guidance PRIVATE Catch2::Catch2WithMain engine-ai engine-net
    engine-entity engine-flight engine-bootstrap)
target_include_directories(test_ballistic_guidance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_ballistic_guidance)

# WeaponRegistry + defaultPayload (#812): a loadout that actually costs the airframe mass and drag.
# fm-trim (#817): the tests LOCK the derived numbers, so they double as the engine's guard against
# someone changing AeroForces.cpp and silently altering every aircraft's performance.
# The generated aircraft manual (#821): retuning the flight model must move the manual.
add_executable(test_aircraft_manual test_aircraft_manual.cpp)
target_link_libraries(test_aircraft_manual PRIVATE Catch2::Catch2WithMain engine-manual engine-entity
    engine-weapon engine-sensor)
catch_discover_tests(test_aircraft_manual)

add_executable(test_fm_trim test_fm_trim.cpp)
target_link_libraries(test_fm_trim PRIVATE Catch2::Catch2WithMain fm-trim-lib)
catch_discover_tests(test_fm_trim)

add_executable(test_loadout test_loadout.cpp)
target_link_libraries(test_loadout PRIVATE Catch2::Catch2WithMain engine-weapon engine-entity engine-flight)
catch_discover_tests(test_loadout)

add_executable(test_sensor_def_parser test_sensor_def_parser.cpp)
target_link_libraries(test_sensor_def_parser PRIVATE Catch2::Catch2WithMain engine-sensor)
catch_discover_tests(test_sensor_def_parser)

add_executable(test_sensor_detection test_sensor_detection.cpp)
target_link_libraries(test_sensor_detection PRIVATE Catch2::Catch2WithMain engine-sensor)
catch_discover_tests(test_sensor_detection)

# #670 acceptance: honest sensing end-to-end through a real WorldBroadcaster tick.
add_executable(test_honest_sensing test_honest_sensing.cpp)
target_link_libraries(test_honest_sensing PRIVATE Catch2::Catch2WithMain engine-net engine-ai engine-sensor
    engine-entity engine-spatial engine-weather engine-job)
catch_discover_tests(test_honest_sensing)

# Builtin surface entities + SAM/AAA engagement controllers (#863): a WB-level engagement test, so it
# links engine-ai (the controllers) + engine-bootstrap (the builtin surface entity defs).
add_executable(test_surface_threats test_surface_threats.cpp)
target_link_libraries(test_surface_threats PRIVATE Catch2::Catch2WithMain engine-net engine-ai engine-sensor
    engine-bootstrap engine-entity engine-spatial engine-weather engine-job)
catch_discover_tests(test_surface_threats)

add_executable(test_sensor_system test_sensor_system.cpp)
target_link_libraries(test_sensor_system PRIVATE Catch2::Catch2WithMain engine-sensor engine-entity engine-spatial)
catch_discover_tests(test_sensor_system)

add_executable(test_aero_forces test_aero_forces.cpp)
target_link_libraries(test_aero_forces PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_aero_forces)

add_executable(test_flight_integrator test_flight_integrator.cpp)
target_link_libraries(test_flight_integrator PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_flight_integrator)

# ArticulationChannels (#1195): the one FlightState -> ArtChannel mapping the wire codec and the
# renderer share. engine-flight only — the mapping deliberately pulls in nothing but the
# dependency-free channel vocabulary.
add_executable(test_articulation_channels test_articulation_channels.cpp)
target_link_libraries(test_articulation_channels PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_articulation_channels)

add_executable(test_rotor_force_models test_rotor_force_models.cpp)
target_link_libraries(test_rotor_force_models PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_rotor_force_models)

add_executable(test_carrier_deck test_carrier_deck.cpp)
target_link_libraries(test_carrier_deck PRIVATE Catch2::Catch2WithMain engine-net engine-entity engine-flight
                                                engine-bootstrap)
catch_discover_tests(test_carrier_deck)

add_executable(test_base_ops test_base_ops.cpp)
target_link_libraries(test_base_ops PRIVATE Catch2::Catch2WithMain engine-net engine-entity engine-flight
                                            engine-weapon engine-bootstrap)
catch_discover_tests(test_base_ops)

add_executable(test_central_gravity test_central_gravity.cpp)
target_link_libraries(test_central_gravity PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_central_gravity)

add_executable(test_geodetic test_geodetic.cpp)
# engine-world for SandboxHome.h — the anchor cases assert against the shipped home (#1211).
target_link_libraries(test_geodetic PRIVATE Catch2::Catch2WithMain engine-flight engine-world)
catch_discover_tests(test_geodetic)

# Local-level (ENU tangent-plane) frame utilities (#470); header-only, glm via engine-flight.
add_executable(test_local_frame test_local_frame.cpp)
target_link_libraries(test_local_frame PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_local_frame)

# Cube-sphere tile addressing + geometry core (#469); header-only, needs glm (engine-render) +
# Geodetic.h (engine/flight, header-only).
add_executable(test_cube_sphere test_cube_sphere.cpp)
target_link_libraries(test_cube_sphere PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_cube_sphere PRIVATE "${CMAKE_SOURCE_DIR}/engine")
catch_discover_tests(test_cube_sphere)

add_executable(test_force_model test_force_model.cpp)
target_link_libraries(test_force_model PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_force_model)

add_executable(test_loop test_loop.cpp)
target_link_libraries(test_loop PRIVATE Catch2::Catch2WithMain engine-loop)
target_include_directories(test_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# RUN_SERIAL: test_loop contains WALL-CLOCK RATE assertions (it sleeps 1 s and asserts the loop fired
# ~60 times). Since #787 made the suite parallel by default, those ran while ~20 other test binaries
# saturated the CPU — so they were no longer measuring the game loop, they were measuring how busy the
# runner was, and they flaked on shared macOS runners. Serialising them restores what the assertion is
# actually for. It costs a few seconds of wall time; a rate test that competes for the CPU is worthless.
catch_discover_tests(test_loop PROPERTIES RUN_SERIAL TRUE)

add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE Catch2::Catch2WithMain engine-sandbox)
target_include_directories(test_sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_sandbox)

add_executable(test_content_system test_content_system.cpp)
target_link_libraries(test_content_system PRIVATE Catch2::Catch2WithMain engine-content engine-difficulty
    engine-sensor)
catch_discover_tests(test_content_system)

add_executable(test_folder_content_pack test_folder_content_pack.cpp)
target_link_libraries(test_folder_content_pack PRIVATE Catch2::Catch2WithMain engine-content)
catch_discover_tests(test_folder_content_pack)

# fl::tomlInt guards five parsers against a real UB bug in toml++ (#824).
# The one strict/tolerant number parse family behind the console, admin, AI-factory, importer and
# mission-runtime parsers (#1244). Header-only, so no engine library is needed.
add_executable(test_parse test_parse.cpp)
target_link_libraries(test_parse PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_parse PRIVATE "${CMAKE_SOURCE_DIR}/engine")
catch_discover_tests(test_parse)

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

add_executable(test_toml_numeric test_toml_numeric.cpp)
target_link_libraries(test_toml_numeric PRIVATE Catch2::Catch2WithMain tomlplusplus::tomlplusplus)
target_include_directories(test_toml_numeric PRIVATE "${CMAKE_SOURCE_DIR}/engine")
catch_discover_tests(test_toml_numeric)

# The shared required/optional TOML field reads behind the seven def parsers (#1245).
add_executable(test_toml_read test_toml_read.cpp)
target_link_libraries(test_toml_read PRIVATE Catch2::Catch2WithMain tomlplusplus::tomlplusplus)
target_include_directories(test_toml_read PRIVATE "${CMAKE_SOURCE_DIR}/engine")
catch_discover_tests(test_toml_read)

add_executable(test_content_index test_content_index.cpp)
target_link_libraries(test_content_index PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_content_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_livery_def test_livery_def.cpp)
target_link_libraries(test_livery_def PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_livery_def PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_livery_def)
catch_discover_tests(test_content_index)

add_executable(test_difficulty test_difficulty.cpp)
target_link_libraries(test_difficulty PRIVATE Catch2::Catch2WithMain engine-difficulty)
target_include_directories(test_difficulty PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_difficulty)

add_executable(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE Catch2::Catch2WithMain engine-input)
catch_discover_tests(test_input)

add_executable(test_localization test_localization.cpp)
target_link_libraries(test_localization PRIVATE Catch2::Catch2WithMain engine-i18n)

# Session-failure i18n coverage (#358): keys present in the repo locale/en/ui.toml + tr fallback.
add_executable(test_session_i18n test_session_i18n.cpp)
target_link_libraries(test_session_i18n PRIVATE Catch2::Catch2WithMain engine-i18n platform-stdfs platform-hal)
target_include_directories(test_session_i18n PRIVATE "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
target_compile_definitions(test_session_i18n PRIVATE FL_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(test_session_i18n)
catch_discover_tests(test_localization)

add_executable(test_firstrun test_firstrun.cpp)
target_link_libraries(test_firstrun PRIVATE Catch2::Catch2WithMain engine-firstrun)
target_include_directories(test_firstrun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_firstrun)

add_executable(test_file_logger test_file_logger.cpp)
target_link_libraries(test_file_logger PRIVATE Catch2::Catch2WithMain platform-file-logger)
catch_discover_tests(test_file_logger)

add_executable(test_crash_reporter test_crash_reporter.cpp)
target_link_libraries(test_crash_reporter PRIVATE Catch2::Catch2WithMain engine-crash platform-file-logger)
target_include_directories(test_crash_reporter PRIVATE ${CMAKE_BINARY_DIR}/engine)
catch_discover_tests(test_crash_reporter)

add_executable(test_userconfig test_userconfig.cpp)
target_link_libraries(test_userconfig PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_userconfig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_userconfig)

add_executable(test_userconfig_enums test_userconfig_enums.cpp)
target_link_libraries(test_userconfig_enums PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_userconfig_enums PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_userconfig_enums)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_settings)

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

add_executable(test_ai_controllers test_ai_controllers.cpp)
target_link_libraries(test_ai_controllers PRIVATE engine-ai engine-entity engine-spatial engine-sensor
    Catch2::Catch2WithMain)
catch_discover_tests(test_ai_controllers)

add_executable(test_state_machine_controller test_state_machine_controller.cpp)
target_link_libraries(test_state_machine_controller PRIVATE engine-ai engine-entity engine-spatial engine-sensor
    Catch2::Catch2WithMain)
catch_discover_tests(test_state_machine_controller)

# Takeoff / landing ground-and-pattern controllers (#701, ATC epic #673).
add_executable(test_takeoff_controller test_takeoff_controller.cpp)
target_link_libraries(test_takeoff_controller PRIVATE engine-ai engine-entity engine-spatial Catch2::Catch2WithMain)
catch_discover_tests(test_takeoff_controller)

add_executable(test_landing_controller test_landing_controller.cpp)
target_link_libraries(test_landing_controller PRIVATE engine-ai engine-entity engine-spatial Catch2::Catch2WithMain)
catch_discover_tests(test_landing_controller)

# ATC service core — facilities, runway sequencing, clearances (#702, ATC epic #673).
add_executable(test_atc_service test_atc_service.cpp)
target_link_libraries(test_atc_service PRIVATE engine-atc engine-world engine-entity engine-spatial
    Catch2::Catch2WithMain)
catch_discover_tests(test_atc_service)

add_executable(test_atc_facility_sequencing test_atc_facility_sequencing.cpp)
target_link_libraries(test_atc_facility_sequencing PRIVATE engine-atc engine-world engine-entity engine-spatial
    Catch2::Catch2WithMain)
catch_discover_tests(test_atc_facility_sequencing)

# The scripted wingman grammar + its two wire messages (#610). Header-only + engine-protocol only.
add_executable(test_wingman_command test_wingman_command.cpp)
target_link_libraries(test_wingman_command PRIVATE engine-protocol engine-world engine-ai Catch2::Catch2WithMain)
catch_discover_tests(test_wingman_command)

# The formation / command tree (#610). Pure stdlib — no entity manager, no network.
add_executable(test_formation_registry test_formation_registry.cpp)
target_link_libraries(test_formation_registry PRIVATE engine-world Catch2::Catch2WithMain)
catch_discover_tests(test_formation_registry)

# FormationController, boresight designation, and the six commands (#610).
add_executable(test_wingman_behavior test_wingman_behavior.cpp)
target_link_libraries(test_wingman_behavior PRIVATE engine-ai engine-entity engine-spatial Catch2::Catch2WithMain)
catch_discover_tests(test_wingman_behavior)

add_executable(test_guidance test_guidance.cpp)
target_link_libraries(test_guidance PRIVATE engine-ai engine-entity engine-spatial Catch2::Catch2WithMain)
catch_discover_tests(test_guidance)

add_executable(test_spatial_index test_spatial_index.cpp)
target_link_libraries(test_spatial_index PRIVATE Catch2::Catch2WithMain engine-spatial)
catch_discover_tests(test_spatial_index)

# Isolated scale/characterisation benchmarks (issue #573). All cases are [.]-hidden so the default
# ctest run skips them; run explicitly via `ctest -R test_entity_scale` or the [scale] tag.
add_executable(test_entity_scale test_entity_scale.cpp)
target_link_libraries(test_entity_scale PRIVATE Catch2::Catch2WithMain engine-entity engine-spatial)
target_include_directories(test_entity_scale PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_entity_scale)

add_executable(test_job_system test_job_system.cpp)
target_link_libraries(test_job_system PRIVATE Catch2::Catch2WithMain engine-job)
catch_discover_tests(test_job_system)

# AdminChannel + AdminChannelRegistry (#1079): the six admin frontends' shared auth, dispatch and
# async-ack drain. engine-net only -- no sockets, no registry, injected clock, so the drain deadline is
# testable without opening a file descriptor (the test_rcon_server recipe generalised).
add_executable(test_admin_channel test_admin_channel.cpp)
target_link_libraries(test_admin_channel PRIVATE Catch2::Catch2WithMain engine-net)
target_include_directories(test_admin_channel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_admin_channel)

# The engine's one JSON escaper, writer set and reader (#1080). Header-only and stdlib-only, so this
# links nothing: engine-protocol's zero-dep rule is the reason the helper is shaped that way.
add_executable(test_json test_json.cpp)
target_link_libraries(test_json PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_json PRIVATE "${CMAKE_SOURCE_DIR}/engine")
catch_discover_tests(test_json)

add_executable(test_faction_registry test_faction_registry.cpp)
target_link_libraries(test_faction_registry PRIVATE Catch2::Catch2WithMain engine-world)
catch_discover_tests(test_faction_registry)

# Airspace alert system (#162): zone geometry, the escalation state machine (driven by an explicit
# simDt, so time is injectable without a clock), the escalation-policy TOML schema, and the
# sample-order independence the pass needs to stay deterministic.
add_executable(test_alert_system test_alert_system.cpp)
target_link_libraries(test_alert_system PRIVATE Catch2::Catch2WithMain engine-world)
catch_discover_tests(test_alert_system)

add_executable(test_game_mode test_game_mode.cpp)
target_link_libraries(test_game_mode PRIVATE Catch2::Catch2WithMain engine-match)
catch_discover_tests(test_game_mode)

add_executable(test_match_controller test_match_controller.cpp)
target_link_libraries(test_match_controller PRIVATE Catch2::Catch2WithMain engine-match)
catch_discover_tests(test_match_controller)

add_executable(test_airport_registry test_airport_registry.cpp)
target_link_libraries(test_airport_registry PRIVATE Catch2::Catch2WithMain engine-world)
catch_discover_tests(test_airport_registry)

add_executable(test_airport_csv_import test_airport_csv_import.cpp)
target_link_libraries(test_airport_csv_import PRIVATE Catch2::Catch2WithMain engine-world)
catch_discover_tests(test_airport_csv_import)

add_executable(test_airport_bootstrap test_airport_bootstrap.cpp)
target_link_libraries(test_airport_bootstrap PRIVATE Catch2::Catch2WithMain engine-world)
target_include_directories(test_airport_bootstrap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_airport_bootstrap)

add_executable(test_crew_partition test_crew_partition.cpp)
target_link_libraries(test_crew_partition PRIVATE Catch2::Catch2WithMain engine-entity)
catch_discover_tests(test_crew_partition)

add_executable(test_airport_renderer test_airport_renderer.cpp)
target_link_libraries(test_airport_renderer PRIVATE Catch2::Catch2WithMain engine-render engine-world)
catch_discover_tests(test_airport_renderer)

add_executable(test_builtin_biomes test_builtin_biomes.cpp)
target_link_libraries(test_builtin_biomes PRIVATE Catch2::Catch2WithMain engine-render)
catch_discover_tests(test_builtin_biomes)

add_executable(test_biome_weights test_biome_weights.cpp)
target_link_libraries(test_biome_weights PRIVATE Catch2::Catch2WithMain engine-render)
catch_discover_tests(test_biome_weights)

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

add_executable(test_subtitle_queue test_subtitle_queue.cpp)
target_link_libraries(test_subtitle_queue PRIVATE Catch2::Catch2WithMain engine-audio)
catch_discover_tests(test_subtitle_queue)

add_executable(test_music_manager test_music_manager.cpp)
target_link_libraries(test_music_manager PRIVATE Catch2::Catch2WithMain engine-audio engine-config)
catch_discover_tests(test_music_manager)

add_executable(test_async_filesystem test_async_filesystem.cpp)
target_link_libraries(test_async_filesystem PRIVATE Catch2::Catch2WithMain platform-hal)
target_include_directories(test_async_filesystem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_async_filesystem)

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

add_executable(test_validate_sensor test_validate_sensor.cpp)
target_link_libraries(test_validate_sensor PRIVATE Catch2::Catch2WithMain validate-sensor-lib)
catch_discover_tests(test_validate_sensor)

add_executable(test_validate_entity test_validate_entity.cpp)
target_link_libraries(test_validate_entity PRIVATE Catch2::Catch2WithMain validate-entity-lib)
catch_discover_tests(test_validate_entity)

add_executable(test_validate_livery test_validate_livery.cpp)
target_link_libraries(test_validate_livery PRIVATE Catch2::Catch2WithMain validate-livery-lib)
target_include_directories(test_validate_livery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_validate_livery)

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
add_executable(test_entity_def_parser_rules test_entity_def_parser_rules.cpp)
target_link_libraries(test_entity_def_parser_rules PRIVATE Catch2::Catch2WithMain engine-entity)
catch_discover_tests(test_entity_def_parser_rules)

add_executable(test_lobby_list_client test_lobby_list_client.cpp)
target_link_libraries(test_lobby_list_client PRIVATE Catch2::Catch2WithMain engine-net platform-hal)
target_include_directories(test_lobby_list_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_lobby_list_client)

add_executable(test_aircraft_manual_vehicles test_aircraft_manual_vehicles.cpp)
target_link_libraries(test_aircraft_manual_vehicles PRIVATE Catch2::Catch2WithMain engine-manual engine-flight
    engine-entity engine-weapon engine-sensor)
catch_discover_tests(test_aircraft_manual_vehicles)

add_executable(test_content_bootstrap_loaders test_content_bootstrap_loaders.cpp)
target_link_libraries(test_content_bootstrap_loaders PRIVATE Catch2::Catch2WithMain engine-bootstrap
    engine-content engine-entity engine-weapon engine-world)
target_include_directories(test_content_bootstrap_loaders PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_content_bootstrap_loaders)

add_executable(test_voice_callout_paths test_voice_callout_paths.cpp)
target_link_libraries(test_voice_callout_paths PRIVATE Catch2::Catch2WithMain engine-audio engine-content
    engine-radio engine-i18n)
target_include_directories(test_voice_callout_paths PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_voice_callout_paths)

add_executable(test_ai_controller_factory test_ai_controller_factory.cpp)
target_link_libraries(test_ai_controller_factory PRIVATE Catch2::Catch2WithMain engine-ai engine-entity
    engine-spatial engine-sensor engine-worldai)
catch_discover_tests(test_ai_controller_factory)

add_executable(test_asset_manager_fallback test_asset_manager_fallback.cpp)
target_link_libraries(test_asset_manager_fallback PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_asset_manager_fallback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_asset_manager_fallback)

add_executable(test_flight_model_parser_validation test_flight_model_parser_validation.cpp)
target_link_libraries(test_flight_model_parser_validation PRIVATE Catch2::Catch2WithMain engine-flight)
catch_discover_tests(test_flight_model_parser_validation)

add_executable(test_mission_parser_validation test_mission_parser_validation.cpp)
target_link_libraries(test_mission_parser_validation PRIVATE Catch2::Catch2WithMain engine-mission)
catch_discover_tests(test_mission_parser_validation)

add_executable(test_campaign_parser_validation test_campaign_parser_validation.cpp)
target_link_libraries(test_campaign_parser_validation PRIVATE Catch2::Catch2WithMain engine-campaign)
catch_discover_tests(test_campaign_parser_validation)

add_executable(test_campaign test_campaign.cpp)
target_link_libraries(test_campaign
    PRIVATE Catch2::Catch2WithMain engine-campaign engine-mission) # engine-mission: a generated sortie must parse
catch_discover_tests(test_campaign)

add_executable(test_campaign_engine_branches test_campaign_engine_branches.cpp)
target_link_libraries(test_campaign_engine_branches PRIVATE Catch2::Catch2WithMain engine-campaign)
catch_discover_tests(test_campaign_engine_branches)

add_executable(test_validate_licenses test_validate_licenses.cpp)
target_link_libraries(test_validate_licenses
    PRIVATE Catch2::Catch2WithMain validate-licenses-lib)
catch_discover_tests(test_validate_licenses)

add_executable(test_validate_mesh test_validate_mesh.cpp)
target_link_libraries(test_validate_mesh
    PRIVATE Catch2::Catch2WithMain validate-mesh-lib)
catch_discover_tests(test_validate_mesh)

# validate-campaign (#847) + the frontline PNG codec.
add_executable(test_frontline_png test_frontline_png.cpp)
target_link_libraries(test_frontline_png PRIVATE Catch2::Catch2WithMain engine-campaign-png)
catch_discover_tests(test_frontline_png)

# The one untrusted-PNG chunk guard behind terrain, frontline and the Vulkan texture loader (#1237).
add_executable(test_png_sanity test_png_sanity.cpp)
target_link_libraries(test_png_sanity PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_png_sanity)

add_executable(test_validate_campaign test_validate_campaign.cpp)
target_link_libraries(test_validate_campaign PRIVATE Catch2::Catch2WithMain validate-campaign-lib engine-campaign-png)
target_include_directories(test_validate_campaign PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_validate_campaign)

# validate-mod (#651): the whole-pack validator.
add_executable(test_validate_mod test_validate_mod.cpp)
target_link_libraries(test_validate_mod PRIVATE Catch2::Catch2WithMain validate-mod-lib engine-crypto)
target_include_directories(test_validate_mod PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_validate_mod)

# Mesh node-tree description for the fl-viewer panel (#838).
add_executable(test_mesh_node_tree test_mesh_node_tree.cpp)
target_link_libraries(test_mesh_node_tree PRIVATE Catch2::Catch2WithMain validate-mesh-lib)
catch_discover_tests(test_mesh_node_tree)

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
add_executable(test_game_protocol test_game_protocol.cpp)
target_link_libraries(test_game_protocol PRIVATE Catch2::Catch2WithMain engine-protocol engine-weather)
catch_discover_tests(test_game_protocol)

add_executable(test_wire_codec test_wire_codec.cpp)
target_link_libraries(test_wire_codec PRIVATE Catch2::Catch2WithMain engine-protocol)
catch_discover_tests(test_wire_codec)

add_executable(test_snapshot_codec test_snapshot_codec.cpp)
target_link_libraries(test_snapshot_codec PRIVATE Catch2::Catch2WithMain engine-protocol)
catch_discover_tests(test_snapshot_codec)

# Scheduler lives in engine-net (SnapshotScheduler.cpp), so this test keeps the engine-net link.
add_executable(test_snapshot_scheduler test_snapshot_scheduler.cpp)
target_link_libraries(test_snapshot_scheduler PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_snapshot_scheduler)

add_executable(test_ack_window test_ack_window.cpp)
target_link_libraries(test_ack_window PRIVATE Catch2::Catch2WithMain engine-protocol)
catch_discover_tests(test_ack_window)

# --- Epic J: in-game voice comms (#499) --------------------------------------------------------
# Opus codec wrapper (#531). Real libopus round trips, so a broken encoder configuration or a
# regressed payload cap fails here rather than on a live server.
add_executable(test_voice_codec test_voice_codec.cpp)
target_link_libraries(test_voice_codec PRIVATE Catch2::Catch2WithMain engine-voice)
catch_discover_tests(test_voice_codec)

# Per-speaker voice jitter buffer (#531; pure, no codec/device).
add_executable(test_voice_jitter test_voice_jitter.cpp)
target_link_libraries(test_voice_jitter PRIVATE Catch2::Catch2WithMain engine-voice)
catch_discover_tests(test_voice_jitter)

# PTT/VOX keying gate (#531; pure, header-only).
add_executable(test_voice_activity test_voice_activity.cpp)
target_link_libraries(test_voice_activity PRIVATE Catch2::Catch2WithMain engine-voice)
catch_discover_tests(test_voice_activity)

# Radio-net table + the builtin zero-config net stack (#531/#532).
add_executable(test_radio_net test_radio_net.cpp)
target_link_libraries(test_radio_net PRIVATE Catch2::Catch2WithMain engine-voice)
catch_discover_tests(test_radio_net)

# Remote voice playback / positional mix (#531) against a queue-tracking IAudio double.
add_executable(test_voice_mixer test_voice_mixer.cpp)
target_link_libraries(test_voice_mixer PRIVATE Catch2::Catch2WithMain engine-voice)
catch_discover_tests(test_voice_mixer)

# The client voice facade (#531). Its five components each had a test; the object that decides WHEN
# to key, WHICH net to key on, and when the capture device runs had none — 0.0% branch coverage
# until #1145, and that is exactly where the player-visible behaviour lives.
add_executable(test_voice_chat test_voice_chat.cpp)
target_link_libraries(test_voice_chat PRIVATE Catch2::Catch2WithMain engine-voice)
target_include_directories(test_voice_chat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_voice_chat)

# Radio presentation DSP (#925): band-limiting, soft clipping, and the byte-stable click/squelch cues.
add_executable(test_radio_dsp test_radio_dsp.cpp)
target_link_libraries(test_radio_dsp PRIVATE Catch2::Catch2WithMain engine-radio)
catch_discover_tests(test_radio_dsp)

# Voice routing (#532): who hears a transmission, as pure functions over a net table + peer list.
add_executable(test_voice_router test_voice_router.cpp)
target_link_libraries(test_voice_router PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_voice_router)

# Server capability vocabulary + role presets (#945; header-only, stdlib).
add_executable(test_capability test_capability.cpp)
target_link_libraries(test_capability PRIVATE Catch2::Catch2WithMain engine-protocol)
catch_discover_tests(test_capability)

# Aggregated world-state surface (#600; pure builder, SDL/ENet-free).
add_executable(test_world_state test_world_state.cpp)
target_link_libraries(test_world_state PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_world_state)

# The match event log (#600): ring bounds + gap detection, JSON schema stability, string escaping,
# and a concurrent append/read case. Links only engine-net (no SDL/ENet), so it joins the TSan set
# alongside test_world_state -- the log is written on the sim thread and read from another.
add_executable(test_match_event_log test_match_event_log.cpp)
target_link_libraries(test_match_event_log PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_match_event_log)

# MissionReport stays valid JSON whatever the mission is named (#1234).
add_executable(test_mission_report test_mission_report.cpp)
target_link_libraries(test_mission_report PRIVATE Catch2::Catch2WithMain engine-mission)
catch_discover_tests(test_mission_report)

# Engine-layer snapshot payload codec (#775; zstd wrapper in engine-compress).
add_executable(test_snapshot_compression test_snapshot_compression.cpp)
target_link_libraries(test_snapshot_compression PRIVATE Catch2::Catch2WithMain engine-compress)
catch_discover_tests(test_snapshot_compression)

# FLIT input-trace codec (writer + reader; header-only, stdlib-only — #560).
add_executable(test_input_trace test_input_trace.cpp)
target_link_libraries(test_input_trace PRIVATE Catch2::Catch2WithMain engine-protocol)
catch_discover_tests(test_input_trace)

add_executable(test_congestion_controller test_congestion_controller.cpp)
target_link_libraries(test_congestion_controller PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_congestion_controller)

add_executable(test_tick_governor test_tick_governor.cpp)
target_link_libraries(test_tick_governor PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_tick_governor)

add_executable(test_performance_overlay test_performance_overlay.cpp)
target_link_libraries(test_performance_overlay PRIVATE Catch2::Catch2WithMain engine-perf)
catch_discover_tests(test_performance_overlay)

add_executable(test_utf8_decode test_utf8_decode.cpp)
target_link_libraries(test_utf8_decode PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_utf8_decode)

# The no-op IAudio (#1117) — the degradation path a machine with no audio device takes, and the
# base every audio test double derives from. Header-only, so platform-hal is the whole dependency.
add_executable(test_null_audio test_null_audio.cpp)
target_link_libraries(test_null_audio PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_null_audio)

# IGui HAL contract test (#156) — drives the scripted NullGui (tests/mock_gui.h), the seam every
# UI-screen test uses. platform-hal supplies IGui.h; the real Dear ImGui backend needs a GPU (not in CI).
add_executable(test_gui test_gui.cpp)
target_link_libraries(test_gui PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_gui)

# JoinServerScreen (#322) — the first IGui screen, driven against NullGui + the HAL mocks.
add_executable(test_join_server_screen test_join_server_screen.cpp)
target_link_libraries(test_join_server_screen PRIVATE Catch2::Catch2WithMain platform-hal game-client)
catch_discover_tests(test_join_server_screen)

# ServerBrowserScreen (#143) — the IGui server-browser list, driven against NullGui.
add_executable(test_server_browser_screen test_server_browser_screen.cpp)
target_link_libraries(test_server_browser_screen PRIVATE Catch2::Catch2WithMain engine-net platform-hal game-client)
catch_discover_tests(test_server_browser_screen)

# ScoreboardOverlay (#647) — the IGui scoreboard table, driven against NullGui.
add_executable(test_scoreboard_overlay test_scoreboard_overlay.cpp)
target_link_libraries(test_scoreboard_overlay PRIVATE Catch2::Catch2WithMain platform-hal game-client)
catch_discover_tests(test_scoreboard_overlay)

# KillFeed (#647) — the HudElement kill-feed ring; header-only, deterministic clock.
add_executable(test_kill_feed test_kill_feed.cpp)
target_link_libraries(test_kill_feed PRIVATE Catch2::Catch2WithMain platform-hal)
target_include_directories(test_kill_feed PRIVATE ${CMAKE_SOURCE_DIR}/game/fighters-legacy)
catch_discover_tests(test_kill_feed)

# ChatOverlay (#646) — the chat display ring + IGui input box, driven against NullGui + a manual clock.
add_executable(test_chat_overlay test_chat_overlay.cpp)
target_link_libraries(test_chat_overlay PRIVATE Catch2::Catch2WithMain engine-net platform-hal game-client)
catch_discover_tests(test_chat_overlay)

# Frame-capture swizzle helper (#912): the pure GPU-independent core of the capture readback.
add_executable(test_capture_frame test_capture_frame.cpp)
target_link_libraries(test_capture_frame PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_capture_frame)

# Capture-boundary scheduler (#916): the pure tick-stream -> output-frame / dup-count logic behind the
# cinematic recorder. Header-only, no HAL — just needs the game include dir for RecordScheduler.h.
add_executable(test_record_scheduler test_record_scheduler.cpp)
target_link_libraries(test_record_scheduler PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_record_scheduler PRIVATE "${CMAKE_SOURCE_DIR}/game/fighters-legacy")
catch_discover_tests(test_record_scheduler)

add_executable(test_hud_align test_hud_align.cpp)
target_link_libraries(test_hud_align PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_hud_align)

# Content → body mesh orientation mapping (#906); header-only, glm via platform-hal.
add_executable(test_mesh_orient test_mesh_orient.cpp)
target_link_libraries(test_mesh_orient PRIVATE Catch2::Catch2WithMain platform-hal)
catch_discover_tests(test_mesh_orient)

add_executable(test_game_console test_game_console.cpp)
target_link_libraries(test_game_console PRIVATE Catch2::Catch2WithMain engine-console engine-entity engine-weather)
target_include_directories(test_game_console PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_game_console)

add_executable(test_console_commands_paths test_console_commands_paths.cpp)
target_link_libraries(test_console_commands_paths PRIVATE Catch2::Catch2WithMain engine-console engine-entity)
catch_discover_tests(test_console_commands_paths)

add_executable(test_command_shell test_command_shell.cpp)
target_link_libraries(test_command_shell PRIVATE Catch2::Catch2WithMain engine-console)
target_include_directories(test_command_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_command_shell)

add_executable(test_world_broadcaster test_world_broadcaster.cpp)
target_link_libraries(test_world_broadcaster PRIVATE Catch2::Catch2WithMain engine-net engine-weather
    engine-bootstrap) # builtinDebugEntityDef/registerBuiltinWeapons — the sandbox arming smoke (#440)
catch_discover_tests(test_world_broadcaster)

# The admission half of the old broadcaster suite (#1085), sharing its fixtures via
# world_broadcaster_test_util.h. Same link set: it drives PeerAdmission through its owner.
add_executable(test_peer_admission test_peer_admission.cpp)
target_link_libraries(test_peer_admission PRIVATE Catch2::Catch2WithMain engine-net engine-weather
    engine-bootstrap)
catch_discover_tests(test_peer_admission)

# The snapshot half (#1086), same arrangement.
add_executable(test_snapshot_pipeline test_snapshot_pipeline.cpp)
target_link_libraries(test_snapshot_pipeline PRIVATE Catch2::Catch2WithMain engine-net engine-weather
    engine-bootstrap)
catch_discover_tests(test_snapshot_pipeline)

# The comms half (#1087), same arrangement.
add_executable(test_session_comms test_session_comms.cpp)
target_link_libraries(test_session_comms PRIVATE Catch2::Catch2WithMain engine-net engine-weather
    engine-bootstrap)
catch_discover_tests(test_session_comms)

add_executable(test_weather test_weather.cpp)
# engine-world for SandboxHome.h — the solar case contrasts the home with the pole it replaced (#1211).
target_link_libraries(test_weather PRIVATE Catch2::Catch2WithMain engine-weather engine-world)

add_executable(test_celestial test_celestial.cpp)
target_link_libraries(test_celestial PRIVATE Catch2::Catch2WithMain engine-weather engine-flight)
catch_discover_tests(test_celestial)
catch_discover_tests(test_weather)

add_executable(test_solar_position test_solar_position.cpp)
target_link_libraries(test_solar_position PRIVATE Catch2::Catch2WithMain engine-weather)
catch_discover_tests(test_solar_position)

add_executable(test_magnetic_model test_magnetic_model.cpp)
target_link_libraries(test_magnetic_model PRIVATE Catch2::Catch2WithMain engine-nav)
catch_discover_tests(test_magnetic_model)

add_executable(test_terrain_chunk_io test_terrain_chunk_io.cpp)
target_link_libraries(test_terrain_chunk_io PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_terrain_chunk_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_terrain_chunk_io)

add_executable(test_terrain_streamer test_terrain_streamer.cpp)
target_link_libraries(test_terrain_streamer PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_terrain_streamer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_terrain_streamer)

add_executable(test_terrain_mesh_builder test_terrain_mesh_builder.cpp)
target_link_libraries(test_terrain_mesh_builder PRIVATE Catch2::Catch2WithMain engine-render)
target_include_directories(test_terrain_mesh_builder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_terrain_mesh_builder)

add_executable(test_config_file test_config_file.cpp)
target_link_libraries(test_config_file PRIVATE Catch2::Catch2WithMain engine-config)
catch_discover_tests(test_config_file)

add_executable(test_network_utils test_network_utils.cpp)
target_link_libraries(test_network_utils PRIVATE Catch2::Catch2WithMain engine-net)
catch_discover_tests(test_network_utils)

add_executable(test_subprocess test_subprocess.cpp)
target_link_libraries(test_subprocess PRIVATE Catch2::Catch2WithMain platform-subprocess)
catch_discover_tests(test_subprocess)

add_executable(test_manifest_sanitization test_manifest_sanitization.cpp)
target_link_libraries(test_manifest_sanitization PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_manifest_sanitization PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_manifest_sanitization)

add_executable(test_trust_framework test_trust_framework.cpp)
target_link_libraries(test_trust_framework PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_trust_framework PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_trust_framework)

add_executable(test_mod_loader_load test_mod_loader_load.cpp)
target_link_libraries(test_mod_loader_load PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_mod_loader_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_mod_loader_load)

add_executable(test_asset_validator test_asset_validator.cpp)
target_link_libraries(test_asset_validator PRIVATE Catch2::Catch2WithMain engine-content)
target_include_directories(test_asset_validator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_asset_validator)

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
add_executable(test_sfx test_sfx.cpp)
target_link_libraries(test_sfx PRIVATE Catch2::Catch2WithMain engine-audio engine-config engine-content)
target_include_directories(test_sfx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_sfx)

add_executable(test_warning_tones test_warning_tones.cpp)
target_link_libraries(test_warning_tones PRIVATE Catch2::Catch2WithMain engine-audio engine-config engine-content)
target_include_directories(test_warning_tones PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_warning_tones)

# #959 — engine/aero doppler layers: pure mapping + byte-stable loops (engine-audio) plus the
# game-layer EngineAudioManager source wiring, compiled in directly.
add_executable(test_engine_audio test_engine_audio.cpp)
target_link_libraries(test_engine_audio PRIVATE Catch2::Catch2WithMain engine-audio engine-config engine-content game-client)
target_include_directories(test_engine_audio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
catch_discover_tests(test_engine_audio)

add_executable(test_sha256 test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE Catch2::Catch2WithMain engine-crypto)
catch_discover_tests(test_sha256)

# The full-entropy token generator behind the admin token, MCP session ids and the pilot GUID (#1233).
add_executable(test_random_token test_random_token.cpp)
target_link_libraries(test_random_token PRIVATE Catch2::Catch2WithMain engine-crypto)
catch_discover_tests(test_random_token)

add_executable(test_content_downloader test_content_downloader.cpp)
# engine-net for LobbyListClient: the #1083 regression drives TWO IHttpClient consumers at once,
# which is the case the old single-slot handler could not serve.
target_link_libraries(test_content_downloader PRIVATE Catch2::Catch2WithMain engine-content engine-crypto
    engine-net)
target_include_directories(test_content_downloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Lobby / server browser (#143): pure parser, registration client, and merge model.
add_executable(test_lobby_list_parser test_lobby_list_parser.cpp)
target_link_libraries(test_lobby_list_parser PRIVATE Catch2::Catch2WithMain engine-net platform-hal)
catch_discover_tests(test_lobby_list_parser)

add_executable(test_lobby_registration test_lobby_registration.cpp)
target_link_libraries(test_lobby_registration PRIVATE Catch2::Catch2WithMain engine-net platform-hal)
target_include_directories(test_lobby_registration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_lobby_registration)

add_executable(test_server_browser_model test_server_browser_model.cpp)
target_link_libraries(test_server_browser_model PRIVATE Catch2::Catch2WithMain engine-net platform-hal)
catch_discover_tests(test_server_browser_model)
catch_discover_tests(test_content_downloader)

add_executable(test_wind_profile test_wind_profile.cpp)
target_link_libraries(test_wind_profile PRIVATE Catch2::Catch2WithMain engine-weather engine-flight)
catch_discover_tests(test_wind_profile)
