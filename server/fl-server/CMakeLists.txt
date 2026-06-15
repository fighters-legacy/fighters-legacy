if(NOT TARGET platform-enet)
    return()
endif()

# Config parsing logic in its own library so tests/test_server_config can link it.
add_library(fl-server-config STATIC server_config.cpp)
target_link_libraries(fl-server-config PUBLIC platform-hal tomlplusplus::tomlplusplus)
target_include_directories(fl-server-config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(fl-server main.cpp ServerCommands.cpp IpListFile.cpp RconServer.cpp)
target_link_libraries(fl-server PRIVATE
    fl-server-config platform-enet engine-net engine-entity engine-loop engine-weather engine-console
    engine-config engine-render engine-content engine-flight platform-sdl3)
target_include_directories(fl-server PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/net"
    "${enet6_SOURCE_DIR}/include")

if(WIN32)
    target_link_libraries(fl-server PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(fl-server PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
