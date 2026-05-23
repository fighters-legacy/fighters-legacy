cmake_minimum_required(VERSION 3.25)
project(fighters-legacy VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(cmake/dependencies.cmake)

enable_testing()

add_subdirectory(engine)
add_subdirectory(platform)
add_subdirectory(tools)
add_subdirectory(tests)
