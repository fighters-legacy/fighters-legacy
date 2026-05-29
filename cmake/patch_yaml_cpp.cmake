# Patches yaml-cpp's CMakeLists.txt for CMake 4.x compatibility.
# yaml-cpp 0.8.0 declares cmake_minimum_required(VERSION 2.8.12) which CMake 4.x
# no longer accepts (minimum allowed is 3.5). Run via FetchContent PATCH_COMMAND
# from within the yaml-cpp source directory.
cmake_minimum_required(VERSION 3.5)
file(READ "CMakeLists.txt" _content)
string(REPLACE
    "cmake_minimum_required(VERSION 2.8.12)"
    "cmake_minimum_required(VERSION 3.5)"
    _content "${_content}")
file(WRITE "CMakeLists.txt" "${_content}")
