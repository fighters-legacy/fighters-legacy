#version 450

// Vertex attributes — interleaved layout matching struct Vertex (VkResources.h)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // w = handedness (+1 or -1)
layout(location = 3) in vec2 inUV;

// Set 0, binding 0: per-frame camera data.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 worldOrigin; // xyz = camera world position, w unused
} camera;

// Push constants: per-object model matrix + material factors.
layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
} push;

layout(location = 0) out vec3  fragWorldPos;           // camera-relative world position
layout(location = 1) out vec3  fragWorldNormal;
layout(location = 2) out vec3  fragWorldTangent;
layout(location = 3) out float fragTangentHandedness;
layout(location = 4) out vec2  fragUV;
// Raw (untransformed) tangent — for terrain (#475) this attribute is not a tangent but packed data:
// .x = WorldCover class, .y = normalized elevation, .zw = spherical detail coordinate (metres). It
// must reach the fragment shader unmodified (no model rotation, no normalize), so pass it raw here.
layout(location = 5) out vec4  fragRawTangent;

void main() {
    // Camera-relative rendering: push.model already encodes the camera-relative transform
    // (the CPU rebases world positions against worldOrigin in double precision before upload),
    // so this position is already relative to the camera origin — do NOT subtract worldOrigin
    // again here.
    vec4 worldPos = push.model * vec4(inPosition, 1.0);

    gl_Position = camera.proj * camera.view * worldPos;

    mat3 normalMat = transpose(inverse(mat3(push.model)));
    fragWorldPos          = worldPos.xyz;
    fragWorldNormal       = normalize(normalMat * inNormal);
    fragWorldTangent      = normalize(mat3(push.model) * inTangent.xyz);
    fragTangentHandedness = inTangent.w;
    fragUV                = inUV;
    fragRawTangent        = inTangent;
}
