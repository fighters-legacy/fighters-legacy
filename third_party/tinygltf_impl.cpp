// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SINGLE tinygltf + stb_image + stb_image_write implementation TU for the whole build (#836).
//
// tinygltf is header-only: whoever defines TINYGLTF_IMPLEMENTATION emits its (non-inline) symbols +
// the bundled stb. Two such TUs in one binary collide at link time. Before this, platform-vulkan
// (VkResources.cpp) and validate-mesh-lib (mesh_validator.cpp) each defined it — fine standalone, but
// fl-viewer links BOTH (the renderer AND the node-tree/validate lib) and the two implementations
// clashed. Centralizing the implementation here, linked by both libraries as `tinygltf-impl`, means a
// binary that pulls either or both gets exactly one copy. The stb symbols stay non-static so
// VkResources' direct stbi_load_from_memory / stbi_write_png calls resolve against this TU.
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>
