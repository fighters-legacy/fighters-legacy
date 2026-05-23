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

# Compile shaders at build time
# glslangValidator is provided by: glslang-tools (Ubuntu), glslang (Homebrew), LunarG SDK (Windows)
find_program(GLSLANG_VALIDATOR glslangValidator
    HINTS "$ENV{VULKAN_SDK}/bin" "${Vulkan_INCLUDE_DIR}/../bin")
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found — install glslang-tools (Ubuntu), glslang (macOS), or the Vulkan SDK (Windows)")
endif()

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_command(
    OUTPUT  "${SHADER_OUT_DIR}/triangle.vert.spv"
    COMMAND ${GLSLANG_VALIDATOR} -V "${SHADER_SRC_DIR}/triangle.vert" -o "${SHADER_OUT_DIR}/triangle.vert.spv"
    DEPENDS "${SHADER_SRC_DIR}/triangle.vert"
)

add_custom_command(
    OUTPUT  "${SHADER_OUT_DIR}/triangle.frag.spv"
    COMMAND ${GLSLANG_VALIDATOR} -V "${SHADER_SRC_DIR}/triangle.frag" -o "${SHADER_OUT_DIR}/triangle.frag.spv"
    DEPENDS "${SHADER_SRC_DIR}/triangle.frag"
)

add_custom_target(vulkan-shaders ALL
    DEPENDS
        "${SHADER_OUT_DIR}/triangle.vert.spv"
        "${SHADER_OUT_DIR}/triangle.frag.spv"
)

add_library(platform-vulkan STATIC VkRenderer.cpp VkWindow.cpp)
add_dependencies(platform-vulkan vulkan-shaders)

target_link_libraries(platform-vulkan
    PUBLIC  platform-hal
    PRIVATE Vulkan::Vulkan SDL3::SDL3
)

target_compile_definitions(platform-vulkan PRIVATE
    $<$<CONFIG:Debug>:FL_VK_VALIDATION>
    FL_SHADER_DIR="${SHADER_OUT_DIR}"
)
