if(NOT TARGET platform-enet)
    return()
endif()

# Config parsing logic in its own library so tests/test_server_config can link it.
add_library(fl-server-config STATIC server_config.cpp)
target_link_libraries(fl-server-config PUBLIC platform-hal tomlplusplus::tomlplusplus)
target_include_directories(fl-server-config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/engine")  # config/TomlNumeric.h — header-only, no link needed

add_executable(fl-server main.cpp ServerCommands.cpp IpListFile.cpp RconServer.cpp)
# platform-net is the backend-selecting facade (enet6 always, GNS when FL_ENABLE_GNS); fl-server
# picks the transport at runtime via createNetwork(cfg.network.transport).
target_link_libraries(fl-server PRIVATE
    fl-server-config platform-net engine-net engine-entity engine-loop engine-weather engine-console
    engine-config engine-render engine-content engine-difficulty engine-flight engine-ai engine-script
    engine-perf engine-weapon engine-bootstrap engine-sensor engine-mission platform-stdfs)
target_include_directories(fl-server PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/net"
    "${enet6_SOURCE_DIR}/include")

if(WIN32)
    target_link_libraries(fl-server PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(fl-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# fl-server ships in the release, and it is not optional: the game spawns it as a co-located
# subprocess for single-player (LocalServer). It was in NEITHER platform's packaging list, which
# means every published release has been unplayable offline. Found while doing #814.
install(TARGETS fl-server RUNTIME DESTINATION . COMPONENT runtime)
