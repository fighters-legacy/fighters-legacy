# platform-stdfs: std::filesystem-backed IFilesystem / IAsyncFilesystem.
# Zero windowing/library dependency (unlike platform-sdl3) — the single
# filesystem HAL backend, used by the headless server and the GUI client alike.
find_package(Threads REQUIRED)

add_library(platform-stdfs STATIC StdFilesystem.cpp StdAsyncFilesystem.cpp)
target_include_directories(platform-stdfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-stdfs
    PUBLIC  platform-hal
    PRIVATE Threads::Threads
)
