# ENet networking backend — implements INetwork
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
# ENet 1.3.17 declares cmake_minimum_required(VERSION 2.6); CMake 4.x dropped
# compatibility with versions below 3.5. Override the policy minimum so the
# FetchContent build succeeds without patching the upstream CMakeLists.txt.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(enet)
unset(CMAKE_POLICY_VERSION_MINIMUM)
unset(CMAKE_COMPILE_WARNING_AS_ERROR)

add_library(platform-enet STATIC ENetNetwork.cpp)
target_link_libraries(platform-enet
    PUBLIC  platform-hal
    PRIVATE enet
)
# ENet 1.3.17 target may not propagate its include path; be explicit.
target_include_directories(platform-enet PRIVATE "${enet_SOURCE_DIR}/include")
