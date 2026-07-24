# platform-hal: INTERFACE library exposing all HAL interface headers.
# Engine core and backends link against this target rather than including
# platform/ headers by path.
add_library(platform-hal INTERFACE)
target_include_directories(platform-hal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-hal INTERFACE glm::glm)

# platform-meshgraph: the glTF scene-graph walk behind node-aware mesh loading (#839). Vulkan-free
# and pure so it is unit-testable without a GPU; platform-vulkan's createMesh uploads what it plans
# and VkRenderer composes its node transforms each frame.
add_library(platform-meshgraph STATIC MeshNodePlan.cpp)
target_include_directories(platform-meshgraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-meshgraph PUBLIC platform-hal tinygltf-impl)
if(TARGET tinygltf)
    target_link_libraries(platform-meshgraph PUBLIC tinygltf)
endif()

# platform-file-logger: production ILogger implementation backed by a file.
add_library(platform-file-logger STATIC FileLogger.cpp)
target_include_directories(platform-file-logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-file-logger PUBLIC platform-hal)

# platform-subprocess: cross-platform child process management (spawn, stdin/stdout pipes).
add_library(platform-subprocess STATIC Subprocess.cpp)
target_include_directories(platform-subprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform-subprocess PUBLIC platform-hal)

# Dear ImGui (#156): the immediate-mode GUI static library, built from the FetchContent-populated
# loose sources + the SDL3/Vulkan backends. Defined here (before the vulkan + gui subdirs) because BOTH
# platform-gui (the IGui backend) and platform-vulkan (which records ImGui draw data into the swapchain
# pass) link it. Vulkan-gated + client-only; the layering guard forbids engine-*/fl-server from reaching
# it. Third-party: warnings-as-error OFF + full suppression, SYSTEM PUBLIC includes.
if(Vulkan_FOUND AND imgui_SOURCE_DIR)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
    set_target_properties(imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    target_compile_options(imgui PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
        $<$<CXX_COMPILER_ID:MSVC>:/W0>
    )
endif()

# HAL backends: std filesystem, SDL3 windowing/input, Vulkan renderer, OpenAL Soft audio, ENet networking
add_subdirectory(stdfs)
add_subdirectory(sdl3)
add_subdirectory(vulkan)
add_subdirectory(gui)
add_subdirectory(openal)
add_subdirectory(net)
add_subdirectory(http)
