# platform-hal: INTERFACE library exposing all HAL interface headers.
# Engine core and backends link against this target rather than including
# platform/ headers by path.
add_library(platform-hal INTERFACE)
target_include_directories(platform-hal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-hal INTERFACE glm::glm)

# platform-file-logger: production ILogger implementation backed by a file.
add_library(platform-file-logger STATIC FileLogger.cpp)
target_include_directories(platform-file-logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-file-logger PUBLIC platform-hal)

# platform-subprocess: cross-platform child process management (spawn, stdin/stdout pipes).
add_library(platform-subprocess STATIC Subprocess.cpp)
target_include_directories(platform-subprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-subprocess PUBLIC platform-hal)

# HAL backends: SDL3 windowing/input, Vulkan renderer, OpenAL Soft audio, ENet networking
add_subdirectory(sdl3)
add_subdirectory(vulkan)
add_subdirectory(openal)
add_subdirectory(net)
