# Test suite — Catch2 via cmake/dependencies.cmake
include(CTest)
include(Catch)

# Tests are registered here as engine code lands:
#   add_executable(test_foo foo_test.cpp)
#   target_link_libraries(test_foo PRIVATE Catch2::Catch2WithMain engine-content)
#   catch_discover_tests(test_foo)

add_executable(test_content_system test_content_system.cpp)
target_link_libraries(test_content_system PRIVATE Catch2::Catch2WithMain engine-content)
catch_discover_tests(test_content_system)

add_executable(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE Catch2::Catch2WithMain engine-input)
catch_discover_tests(test_input)

add_executable(test_localization test_localization.cpp)
target_link_libraries(test_localization PRIVATE Catch2::Catch2WithMain engine-i18n)
catch_discover_tests(test_localization)

add_executable(test_firstrun test_firstrun.cpp)
target_link_libraries(test_firstrun PRIVATE Catch2::Catch2WithMain engine-firstrun)
catch_discover_tests(test_firstrun)

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
