cmake_minimum_required(VERSION 3.25)
project(fighters-legacy VERSION 0.2.6 LANGUAGES C CXX)

execute_process(
    COMMAND git rev-parse --short HEAD
    OUTPUT_VARIABLE FL_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
if(NOT FL_GIT_HASH)
    set(FL_GIT_HASH "unknown")
endif()
configure_file(cmake/version.h.in engine/Version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# GameNetworkingSockets transport backend (#507). ON pulls protobuf (FetchContent) + OpenSSL
# (system) into the game/fl-server build. OFF yields an enet6-only build for lean tool/CI configs.
# Auto-disabled in dependencies.cmake if OpenSSL is not found.
option(FL_ENABLE_GNS "Build the GameNetworkingSockets transport backend (protobuf + OpenSSL)" ON)

include(cmake/dependencies.cmake)

enable_testing()

add_subdirectory(engine)
add_subdirectory(platform)
add_subdirectory(game)
add_subdirectory(server)
add_subdirectory(tools)
add_subdirectory(tests)
