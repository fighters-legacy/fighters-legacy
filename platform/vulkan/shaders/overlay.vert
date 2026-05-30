#version 450

layout(push_constant) uniform PC {
    vec2 screenSize; // (width, height) in pixels
} pc;

layout(location = 0) in vec2 inPos;   // pixel coordinates (top-left origin)
layout(location = 1) in vec2 inUV;    // font atlas UV [0,1]
layout(location = 2) in vec4 inColor; // RGBA tint (white for normal text)

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    // Convert pixel position to NDC: x in [-1,1], y in [-1,1] (Vulkan Y-down).
    vec2 ndc = (inPos / pc.screenSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outUV = inUV;
    outColor = inColor;
}
