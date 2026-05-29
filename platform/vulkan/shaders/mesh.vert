#version 450

// Vertex attributes — interleaved layout matching struct Vertex (VkResources.h)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;  // w = handedness (+1 or -1)
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

layout(location = 0) out vec3 fragWorldNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec4 fragBaseColorFactor;

void main() {
    // Camera-relative rendering: rebase world position to camera origin so
    // float32 precision is safe at arbitrary theater scale.
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    worldPos.xyz -= camera.worldOrigin.xyz;

    gl_Position = camera.proj * camera.view * worldPos;

    mat3 normalMat = transpose(inverse(mat3(push.model)));
    fragWorldNormal     = normalize(normalMat * inNormal);
    fragUV              = inUV;
    fragBaseColorFactor = push.baseColorFactor;
}
