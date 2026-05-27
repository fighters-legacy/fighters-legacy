# Test suite — Catch2 via cmake/dependencies.cmake
include(CTest)
include(Catch)

# Tests are registered here as engine code lands:
#   add_executable(test_foo foo_test.cpp)
#   target_link_libraries(test_foo PRIVATE Catch2::Catch2WithMain engine-content)
#   catch_discover_tests(test_foo)

add_executable(test_loop test_loop.cpp)
target_link_libraries(test_loop PRIVATE Catch2::Catch2WithMain engine-loop)
target_include_directories(test_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_loop)

add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE Catch2::Catch2WithMain engine-sandbox)
target_include_directories(test_sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_sandbox)

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
target_include_directories(test_firstrun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_firstrun)

add_executable(test_file_logger test_file_logger.cpp)
target_link_libraries(test_file_logger PRIVATE Catch2::Catch2WithMain platform-file-logger)
catch_discover_tests(test_file_logger)

add_executable(test_crash_reporter test_crash_reporter.cpp)
target_link_libraries(test_crash_reporter PRIVATE Catch2::Catch2WithMain engine-crash platform-file-logger)
target_include_directories(test_crash_reporter PRIVATE ${CMAKE_BINARY_DIR}/engine)
catch_discover_tests(test_crash_reporter)

add_executable(test_userconfig test_userconfig.cpp)
target_link_libraries(test_userconfig PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_userconfig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_userconfig)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE Catch2::Catch2WithMain engine-config)
target_include_directories(test_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(test_settings)

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

    add_executable(test_server_config test_server_config.cpp)
    target_link_libraries(test_server_config PRIVATE Catch2::Catch2WithMain fl-server-config)
    target_include_directories(test_server_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(test_server_config)
endif()

add_executable(test_audio test_audio.cpp)
target_link_libraries(test_audio PRIVATE Catch2::Catch2WithMain platform-openal OpenAL::OpenAL)
target_include_directories(test_audio PRIVATE "${CMAKE_SOURCE_DIR}/platform/openal")
if(WIN32)
    add_custom_command(TARGET test_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL::OpenAL>
            $<TARGET_FILE_DIR:test_audio>
    )
endif()
catch_discover_tests(test_audio DISCOVERY_MODE PRE_TEST)

add_executable(test_sdl3 test_sdl3.cpp)
target_link_libraries(test_sdl3 PRIVATE Catch2::Catch2WithMain platform-sdl3)
target_include_directories(test_sdl3 PRIVATE "${CMAKE_SOURCE_DIR}/platform/sdl3")
if(WIN32)
    add_custom_command(TARGET test_sdl3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:test_sdl3>
    )
endif()
catch_discover_tests(test_sdl3 DISCOVERY_MODE PRE_TEST)
