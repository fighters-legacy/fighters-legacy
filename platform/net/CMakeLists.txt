# enet6 networking backend — implements INetwork
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
# enet6 uses add_library() without STATIC/SHARED so it follows BUILD_SHARED_LIBS.
# Force static to avoid an enet6.dll/.so runtime dependency — mirrors the
# openal-soft pattern in dependencies.cmake.
set(_fl_enet_save_bsl "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(enet6)
unset(BUILD_SHARED_LIBS CACHE)
if(_fl_enet_save_bsl)
    set(BUILD_SHARED_LIBS "${_fl_enet_save_bsl}")
endif()
unset(_fl_enet_save_bsl)
unset(CMAKE_COMPILE_WARNING_AS_ERROR)

add_library(platform-enet STATIC ENetNetwork.cpp ENetNetworkFactory.cpp)
target_link_libraries(platform-enet
    PUBLIC  platform-hal
    PRIVATE enet6
)
# enet6 still requires ws2_32/winmm on Windows (same as enet 1.3.x).
if(WIN32)
    target_link_libraries(platform-enet PRIVATE ws2_32 winmm)
endif()
# enet6 target may not propagate its include path; be explicit.
target_include_directories(platform-enet PRIVATE "${enet6_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------
# GameNetworkingSockets backend — implements INetwork (#507). Gated on FL_ENABLE_GNS.
# Pulls protobuf (FetchContent) + OpenSSL (system) via the declarations in
# cmake/dependencies.cmake. Kept in its own static lib so the heavy protobuf/GNS chain
# stays out of the enet6-only build (net_check / bot_swarm / lean CI).
# ---------------------------------------------------------------------------
if(FL_ENABLE_GNS)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    # protobuf is resolved in cmake/dependencies.cmake: system-preferred (FL_GNS_PROTOBUF_SYSTEM ON),
    # else built from the FetchContent fallback first so GNS's find_package(Protobuf) resolves it.
    if(NOT FL_GNS_PROTOBUF_SYSTEM)
        FetchContent_MakeAvailable(protobuf)
    endif()
    FetchContent_MakeAvailable(GameNetworkingSockets)
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)

    add_library(platform-gns STATIC GnsNetwork.cpp GnsNetworkFactory.cpp)
    target_compile_definitions(platform-gns PRIVATE FL_ENABLE_GNS)
    target_link_libraries(platform-gns
        PUBLIC  platform-hal
        PRIVATE GameNetworkingSockets::static OpenSSL::SSL OpenSSL::Crypto
    )
    # GNS/protobuf headers are third-party; keep them off our -Werror path.
    target_include_directories(platform-gns SYSTEM PRIVATE
        "${gamenetworkingsockets_SOURCE_DIR}/include")
    if(WIN32)
        # GNS on Windows needs the socket + crypto system libs.
        target_link_libraries(platform-gns PRIVATE ws2_32 crypt32 iphlpapi winmm)
    endif()
endif()

# ---------------------------------------------------------------------------
# platform-net — backend-selecting facade. Consumers (game, fl-server) link THIS and
# call createNetwork(TransportKind); they never see a concrete backend header. Links
# enet6 always, GNS conditionally.
# ---------------------------------------------------------------------------
add_library(platform-net STATIC NetworkFactory.cpp)
target_link_libraries(platform-net PUBLIC platform-hal PRIVATE platform-enet)
if(FL_ENABLE_GNS)
    target_compile_definitions(platform-net PRIVATE FL_ENABLE_GNS)
    target_link_libraries(platform-net PRIVATE platform-gns)
endif()
