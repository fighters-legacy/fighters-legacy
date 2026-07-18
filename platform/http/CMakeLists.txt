# SPDX-License-Identifier: GPL-3.0-or-later
# HTTP HAL (#490). The factory always builds; the libcurl backend compiles only when a system libcurl
# is present (find_package(CURL)). Without it, createHttpClient() returns nullptr — a lean tools build
# or a box without libcurl-dev still links platform-http cleanly. TLS/CA come from the system libcurl
# (accepted as a dynamic dep like the Vulkan loader; the static-link CI check excludes it).
find_package(CURL QUIET)

set(HTTP_SOURCES CurlHttpClientFactory.cpp)
if(CURL_FOUND)
    list(APPEND HTTP_SOURCES CurlHttpClient.cpp)
endif()

add_library(platform-http STATIC ${HTTP_SOURCES})
target_link_libraries(platform-http PUBLIC platform-hal)

if(CURL_FOUND)
    target_compile_definitions(platform-http PRIVATE FL_HAVE_CURL)
    target_link_libraries(platform-http PRIVATE CURL::libcurl)
    message(STATUS "platform-http: libcurl backend enabled (${CURL_VERSION_STRING})")
else()
    message(STATUS "platform-http: libcurl not found — HTTP backend disabled (createHttpClient -> nullptr)")
endif()
