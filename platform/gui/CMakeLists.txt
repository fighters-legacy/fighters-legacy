# SPDX-License-Identifier: GPL-3.0-or-later
#
# platform-gui — the Dear ImGui reference backend behind the IGui HAL (#156, from epic #593).
# Client-only and Vulkan-gated: skipped entirely on a headless / no-Vulkan build (fl-server never
# links it; the layering guard forbids engine-* / fl-server from reaching it). The `imgui` static
# library itself is defined one level up in platform/CMakeLists.txt (shared with platform-vulkan).

if(NOT Vulkan_FOUND)
    message(STATUS "platform-gui: Vulkan SDK not found — skipping")
    return()
endif()

if(NOT TARGET imgui)
    message(FATAL_ERROR "platform-gui: the imgui target was not defined (see platform/CMakeLists.txt)")
endif()

add_library(platform-gui STATIC
    ImGuiGui.cpp
)
target_link_libraries(platform-gui
    PUBLIC  platform-hal
    PRIVATE imgui SDL3::SDL3 Vulkan::Vulkan
)
