#version 450

layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    float mask = texture(fontAtlas, inUV).r;
    outColor = vec4(inColor.rgb, inColor.a * mask);
}
