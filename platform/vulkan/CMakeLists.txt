# Vulkan renderer backend — implements IRenderer
# Guard: skip if Vulkan SDK not present (CI without SDK still builds clean)
if(NOT Vulkan_FOUND)
    message(STATUS "platform-vulkan: Vulkan SDK not found — skipping")
    return()
endif()

if(NOT SDL3_FOUND)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    FetchContent_MakeAvailable(SDL3)
    unset(CMAKE_COMPILE_WARNING_AS_ERROR)
endif()

# ---------------------------------------------------------------------------
# Shader compilation
# glslangValidator is provided by: glslang-tools (Ubuntu), glslang (Homebrew),
# LunarG SDK (Windows)
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR glslangValidator
    HINTS "$ENV{VULKAN_SDK}/bin" "${Vulkan_INCLUDE_DIR}/../bin")
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "glslangValidator not found — install glslang-tools (Ubuntu), "
        "glslang (macOS), or the Vulkan SDK (Windows)")
endif()

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

# List every GLSL source; add new shaders here.
set(GLSL_SHADERS
    mesh.vert
    mesh.frag
    shadow.vert
    sky.frag
    tonemap.vert
    tonemap.frag
    bloom_threshold.frag
    bloom_blur.frag
    particle_sim.comp
    particle.vert
    particle.frag
    overlay.vert
    overlay.frag
)

set(SPIRV_FILES "")
foreach(SHADER IN LISTS GLSL_SHADERS)
    set(SRC "${SHADER_SRC_DIR}/${SHADER}")
    set(SPV "${SHADER_OUT_DIR}/${SHADER}.spv")
    add_custom_command(
        OUTPUT  "${SPV}"
        COMMAND ${GLSLANG_VALIDATOR} -V "${SRC}" -o "${SPV}"
        DEPENDS "${SRC}"
        COMMENT "Compiling shader ${SHADER}"
        VERBATIM
    )
    list(APPEND SPIRV_FILES "${SPV}")
endforeach()

add_custom_target(vulkan-shaders ALL DEPENDS ${SPIRV_FILES})

# ---------------------------------------------------------------------------
# Renderer library
# ---------------------------------------------------------------------------
add_library(platform-vulkan STATIC
    VkRenderer.cpp
    VkResources.cpp
    VkWindow.cpp
    UnifontBitmap.cpp
)
# UnifontBitmap.cpp contains a 1 MB aggregate initializer; MSVC's default COFF
# object format cap (65 536 sections) is exceeded without /bigobj.
if(MSVC)
    set_source_files_properties(UnifontBitmap.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
add_dependencies(platform-vulkan vulkan-shaders)

# VulkanMemoryAllocator — header-only, populated in cmake/dependencies.cmake.
if(NOT TARGET VulkanMemoryAllocator)
    message(FATAL_ERROR "VulkanMemoryAllocator target not found — check cmake/dependencies.cmake")
endif()

# KTX-Software — read-only static library for KTX2 / Basis Universal transcode.
# The target may be ktx_read (static) or ktx (full).  Prefer ktx_read.
if(TARGET ktx_read)
    set(_ktx_target ktx_read)
elseif(TARGET ktx)
    set(_ktx_target ktx)
else()
    message(FATAL_ERROR "KTX-Software target not found — check cmake/dependencies.cmake")
endif()

target_link_libraries(platform-vulkan
    PUBLIC  platform-hal
    PRIVATE Vulkan::Vulkan SDL3::SDL3
            VulkanMemoryAllocator
            ${_ktx_target}
            tinygltf
)

target_compile_definitions(platform-vulkan PRIVATE
    $<$<CONFIG:Debug>:FL_VK_VALIDATION>
    # Build-tree shader dir — runtime path discovery uses SDL_GetBasePath() first.
    FL_SHADER_DIR="${SHADER_OUT_DIR}"
)

# ---------------------------------------------------------------------------
# Install: stage compiled SPIR-V next to the binary so the runtime path
# resolver finds them at share/fighters-legacy/shaders/ (Linux/Windows) or
# Contents/Resources/shaders/ (macOS .app).
# ---------------------------------------------------------------------------
install(FILES ${SPIRV_FILES}
    DESTINATION share/fighters-legacy/shaders
)
