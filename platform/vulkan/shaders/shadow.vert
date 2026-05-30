#version 450

// Depth-only shadow pass.  Only position is read from the interleaved vertex
// buffer; stride matches struct Vertex (48 bytes) but only location 0 is used.

layout(location = 0) in vec3 inPosition;

layout(set = 0, binding = 0) uniform ShadowUBO {
    mat4 lightViewProj[4]; // one per cascade
    vec4 splitDepths;      // x/y/z = view-space end of cascades 0/1/2, w = shadow far
} shadow;

layout(push_constant) uniform PushConstants {
    mat4 model;
    uint cascadeIdx;
} push;

void main() {
    gl_Position = shadow.lightViewProj[push.cascadeIdx] * push.model * vec4(inPosition, 1.0);
}
