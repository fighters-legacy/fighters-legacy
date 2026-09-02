# The server persistence store (#533, plan #1366 decision D24).
#
# Its own target under server/, not a platform-* HAL and not an engine-* library: nothing in
# engine/ consumes it, and cmake/layering.cmake's rule 8 pins that both ways -- no engine-* target
# may reach a SQL engine, and this target may not reach a client backend.
#
# It links no fl-server code on purpose. The store knows nothing about ServerConfig, commands or
# peers; ServerRuntime translates config into the option structs. That is what makes it testable
# without standing up a server, and what would make a future repo split cut here cleanly.
add_library(fl-server-persistence STATIC
    Migrations.cpp
    AsyncWriter.cpp
    SqliteStore.cpp
    PostgresStore.cpp
    NullStore.cpp)

target_include_directories(fl-server-persistence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fl-server-persistence
    PUBLIC platform-hal   # ILogger, and nothing else from the HAL
    PRIVATE fl::sqlite3)

# Always defined, with a value, so PostgresStore.cpp's `#if FL_WITH_POSTGRES` reads a real macro in
# both configurations rather than relying on an undefined identifier evaluating to 0. PUBLIC so a
# test can branch on the same flag the implementation did.
target_compile_definitions(fl-server-persistence PUBLIC FL_WITH_POSTGRES=$<BOOL:${FL_WITH_POSTGRES}>)

if(FL_WITH_POSTGRES)
    target_link_libraries(fl-server-persistence PRIVATE fl::libpq)
endif()

find_package(Threads REQUIRED)
target_link_libraries(fl-server-persistence PUBLIC Threads::Threads)
