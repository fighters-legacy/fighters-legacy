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

if(TARGET platform-enet)
    add_executable(test_network test_network.cpp)
    target_link_libraries(test_network PRIVATE Catch2::Catch2WithMain platform-enet)
    target_include_directories(test_network PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/net"
    )
    catch_discover_tests(test_network)
endif()
