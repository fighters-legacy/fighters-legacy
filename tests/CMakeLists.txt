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
