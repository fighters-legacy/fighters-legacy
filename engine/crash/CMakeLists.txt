add_library(engine-crash STATIC CrashReporter.cpp)
target_include_directories(engine-crash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine-crash PUBLIC platform-hal platform-file-logger)
target_include_directories(engine-crash PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_BINARY_DIR}/engine)
