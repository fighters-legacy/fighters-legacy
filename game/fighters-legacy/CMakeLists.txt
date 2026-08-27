# game-client — every client source except main.cpp (#1067).
#
# Before this library existed, tests/CMakeLists.txt re-listed these sources 80 times with a private
# copy of the include dirs and link set each time, and cmake/layering.cmake could not see client code
# at all. Both are now one link edge.
#
# The library configures in EVERY leg, including the ones with no Vulkan SDK (asan, tsan, fuzz, the
# scale gate) where platform-vulkan and platform-gui do not exist as targets. That is possible because
# the client names no render or GUI backend: main.cpp injects both through ClientBackends
# (see ClientBackends.h), the same discipline as the createNetwork() transport facade. Without it the
# ~25 client tests would have vanished from the sanitizer legs the moment they moved onto the library.
add_library(game-client STATIC
    Game.cpp
    ClientEffectRouter.cpp
    ClientNetEventHandler.cpp
    ClientFlightModelResolver.cpp
    ManualOverlay.cpp
    ClientPrediction.cpp
    Autopilot.cpp
    TargetDesignation.cpp
    PadlockTracker.cpp
    HeadTracker.cpp
    CameraInput.cpp
    FlightInputCollector.cpp
    HapticController.cpp
    EngineAudioManager.cpp
    VideoEncoderPipe.cpp
    LocalServer.cpp
    ReplayPlayer.cpp
    ReplaySelectScreen.cpp
    ScreenManager.cpp
    MainMenuScreen.cpp
    JoinServerScreen.cpp
    ServerBrowserScreen.cpp
    LoadingScreen.cpp
    MissionSelectScreen.cpp
    MissionBriefScreen.cpp
    SettingsScreen.cpp
    FlightScreen.cpp
    WingmanMenu.cpp
    VoiceCommandCapture.cpp   # #935: PTT -> STT -> phrase match -> the same MsgWingmanCommand the menu sends
    CommsMenu.cpp
    PauseMenuScreen.cpp
    DebriefScreen.cpp
    ScoreboardOverlay.cpp
    ChatOverlay.cpp
    GmMapOverlay.cpp
)
# PUBLIC: a consumer (the executable, a test) compiles against these headers, so the include dirs and
# the engine link set travel with the library rather than being restated per target.
target_link_libraries(game-client PUBLIC
    engine-crash
    engine-crypto  # randomHexToken for the single-player admin token (#1233)
    engine-config
    engine-i18n
    engine-flight
    engine-difficulty
    engine-firstrun
    engine-content
    engine-input
    engine-sandbox
    engine-loop
    engine-entity
    engine-net
    engine-replay  # .flrep playback (#41); the reader only -- no server sim comes with it
    engine-weather
    engine-perf
    engine-console
    engine-manual
    engine-bootstrap
    engine-mission
    engine-weapon
    engine-sensor
    engine-voice     # Epic J: Opus capture/playback + the radio-net mix (#531/#532)
    engine-world     # AirportRegistry + runway terrain flattening (#486)
    engine-audio
    engine-ai
    engine-spatial
    engine-render
    platform-file-logger
    platform-subprocess
    platform-stdfs
    platform-sdl3
    platform-net
    platform-http
    platform-stt   # #935: speech-to-text HAL (null backend unless FL_ENABLE_WHISPER)
    platform-hal
    SDL3::SDL3)
target_include_directories(game-client PUBLIC
    ${CMAKE_BINARY_DIR}/engine
    "${CMAKE_SOURCE_DIR}/platform/net"
    "${CMAKE_SOURCE_DIR}/platform/sdl3"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(game-client PRIVATE
    "${enet6_SOURCE_DIR}/include")
# Dev-build server path: embed the build-tree fl-server path at compile time. In release packages
# fl-server is co-located with fighters-legacy and SDL_GetBasePath() is used instead
# (see LocalServer.cpp, where the definition is #ifdef-guarded).
if(TARGET fl-server)
    target_compile_definitions(game-client PRIVATE
        FL_SERVER_DEV_PATH="$<TARGET_FILE_DIR:fl-server>/fl-server")
endif()
if(WIN32)
    target_link_libraries(game-client PUBLIC ws2_32 winmm)
endif()
if(MSVC)
    target_compile_definitions(game-client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(TARGET platform-sdl3 AND TARGET platform-vulkan)
    # A thin main: argument help, then the backend selection game-client deliberately does not make.
    add_executable(fighters-legacy main.cpp)
    target_link_libraries(fighters-legacy PRIVATE
        game-client
        platform-vulkan
        platform-gui    # #156: Dear ImGui backend behind the IGui HAL
        platform-openal # OpenAL Soft behind the IAudio HAL
        Vulkan::Vulkan)
    target_include_directories(fighters-legacy PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/vulkan"
        "${CMAKE_SOURCE_DIR}/platform/gui"
        "${CMAKE_SOURCE_DIR}/platform/openal")
    # Inside the guard: the game target only exists when SDL3 and Vulkan are available. Several CI
    # legs configure without them (lint, asan, tsan, fuzz, the scale gate), and an install() naming a
    # target that does not exist is a hard CONFIGURE error, not a skipped rule.
    install(TARGETS fighters-legacy RUNTIME DESTINATION . COMPONENT runtime)

    if(MSVC)
        target_compile_definitions(fighters-legacy PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
else()
    # SAY SO (#1354). Skipping the client silently is what made a Vulkan-off reconfigure of an
    # existing build/release tree invisible: the target simply stopped existing, `cmake --build`
    # kept exiting 0, and the binary from before the reconfigure sat there being run by every
    # subsequent verification — twelve days stale, in the case that found this. Configure-time is
    # the only moment the cause is still on screen.
    set(_fl_client_missing "")
    if(NOT TARGET platform-sdl3)
        list(APPEND _fl_client_missing "SDL3")
    endif()
    if(NOT TARGET platform-vulkan)
        list(APPEND _fl_client_missing "Vulkan")
    endif()
    list(JOIN _fl_client_missing ", " _fl_client_missing_str)
    message(WARNING
        "Game client target `fighters-legacy` will NOT be built: ${_fl_client_missing_str} "
        "unavailable in this configuration. This is expected for the headless presets "
        "(release-headless, asan, tsan, fuzz) and for CI lint legs. If you did not intend it, note "
        "that any fighters-legacy binary already present in ${CMAKE_BINARY_DIR} is now STALE and "
        "will not be rebuilt.")
    unset(_fl_client_missing)
    unset(_fl_client_missing_str)
endif()
