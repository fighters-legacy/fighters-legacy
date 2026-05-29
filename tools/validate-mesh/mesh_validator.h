// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

struct MeshValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates a glTF 2.0 file (.glb or .gltf) against engine mesh conventions
// documented in docs/modding/3d-models.md.
//
// validateMesh also discovers and validates LOD sibling files in the same directory
// (e.g. fa18c_lod0.glb when given fa18c.glb).
MeshValidationResult validateMesh(const std::string& filePath);

// Validates glTF 2.0 JSON from an in-memory string — used by unit tests.
MeshValidationResult validateMeshFromJson(std::string_view jsonContent);
