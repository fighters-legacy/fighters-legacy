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

add_executable(test_audio test_audio.cpp)
# Same header-propagation issue as audio_test: OALAudio.h includes <AL/al.h>.
target_link_libraries(test_audio PRIVATE Catch2::Catch2WithMain platform-openal OpenAL::OpenAL)
target_include_directories(test_audio PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/openal"
)
# On Windows, OpenAL Soft builds as OpenAL32.dll. copy_if_different it next to
# the test binary so catch_discover_tests (which runs the exe at build time) and
# ctest can both find it without requiring PATH manipulation.
if(WIN32)
    add_custom_command(TARGET test_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL::OpenAL>
            $<TARGET_FILE_DIR:test_audio>
    )
endif()
catch_discover_tests(test_audio)
