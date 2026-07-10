// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: validateMeshFromJson — the in-memory glTF 2.0 validator (tinygltf) behind the
// validate-mesh tool. Content-pack meshes are attacker-controlled JSON; tinygltf's JSON + base64 +
// buffer parsing is a broad surface. Wrapped defensively in try/catch; the invariant is no OOB
// read / no UB for any malformed glTF JSON.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "mesh_validator.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view json(reinterpret_cast<const char*>(data), size);
    try {
        (void)fl::validateMeshFromJson(json);
    } catch (const std::exception&) {
        // Defensive: tinygltf generally reports errors in-band, but guard anyway.
    }
    return 0;
}
