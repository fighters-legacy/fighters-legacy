if(NOT TARGET platform-enet)
    return()
endif()

# Config parsing logic in its own library so tests/test_server_config can link it.
add_library(fl-server-config STATIC server_config.cpp)
target_link_libraries(fl-server-config PUBLIC platform-hal tomlplusplus::tomlplusplus)
target_include_directories(fl-server-config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/engine")  # config/TomlNumeric.h — header-only, no link needed

# fl-server-lib — every server source except main.cpp (#1067).
#
# It exists for the same two reasons game-client does: tests and fuzz harnesses re-listed these
# sources with a private include/link set each time, and cmake/layering.cmake could not see server
# code at all. platform-net is the backend-selecting facade (enet6 always, GNS when FL_ENABLE_GNS);
# the transport is picked at runtime via createNetwork(cfg.network.transport).
add_library(fl-server-lib STATIC
    ServerCommands.cpp
    IpListFile.cpp
    RconServer.cpp
    HttpAdminServer.cpp
    StdinCommandReader.cpp
    ReplayRecorder.cpp
    HttpAdminAuth.cpp
    McpProtocol.cpp
    McpEndpoint.cpp)
# PUBLIC so consumers (the thin main, tests, fuzz harnesses) inherit the headers and link set.
target_link_libraries(fl-server-lib PUBLIC
    fl-server-config platform-net engine-net engine-entity engine-loop engine-weather engine-console
    engine-config engine-render engine-content engine-difficulty engine-flight engine-ai engine-script
    engine-replay engine-perf engine-weapon engine-bootstrap engine-sensor engine-mission engine-campaign
    engine-campaign-png engine-match engine-worldai
    platform-stdfs platform-http
    # cpp-httplib: the embedded REST admin API + /health (#233), and the MCP endpoint (#601).
    # Confined to the server product library by cmake/layering.cmake -- no engine-* target may link it.
    httplib::httplib)
target_include_directories(fl-server-lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/engine"  # generated Version.h: the engine version stamped into recordings
    "${CMAKE_SOURCE_DIR}/platform/net"
    "${CMAKE_SOURCE_DIR}/platform/http")
target_include_directories(fl-server-lib PRIVATE "${enet6_SOURCE_DIR}/include")

if(WIN32)
    target_link_libraries(fl-server-lib PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_definitions(fl-server-lib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(fl-server main.cpp)
target_link_libraries(fl-server PRIVATE fl-server-lib)
target_include_directories(fl-server PRIVATE "${enet6_SOURCE_DIR}/include")

if(MSVC)
    target_compile_definitions(fl-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# fl-server ships in the release, and it is not optional: the game spawns it as a co-located
# subprocess for single-player (LocalServer). It was in NEITHER platform's packaging list, which
# means every published release has been unplayable offline. Found while doing #814.
install(TARGETS fl-server RUNTIME DESTINATION . COMPONENT runtime)

# Runtime data files read from PathDomain::Assets at startup (co-located with the binaries via
# SDL_GetBasePath). This directory was NEVER installed, so every release shipped without it: the
# OurAirports airport database (#486) would not load, and the bundled WMM.COF reference copy (#483)
# was missing. Found while doing #486. Both fl-server and the game read `data/`, so it installs once
# here (fl-server always configures, unlike the SDL/Vulkan-gated game target). COMPONENT-scoped so it
# rides the same runtime archive and no FetchContent dep's data/ leaks in.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/ DESTINATION data COMPONENT runtime
        FILES_MATCHING PATTERN "*.csv" PATTERN "*.COF")
