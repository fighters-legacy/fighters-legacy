#version 450

// Bloom brightness threshold pass.
// Samples the HDR buffer (linear light, pre-tonemap) and outputs only
// pixels whose luminance exceeds the threshold so they contribute to bloom.
// Rendered at half resolution into the bloom attachment.

layout(location = 0) in  vec2 texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;

layout(push_constant) uniform BloomPush {
    float texelSizeX;
    float texelSizeY;
    uint  isVertical; // unused in this pass
    float _pad;
} push;

void main() {
    vec3 hdr = texture(hdrBuffer, texCoord).rgb;

    // Soft-knee threshold: bloom kicks in above 0.70 nits (HDR linear). Lowered from 0.80 to
    // capture terrain/entity specular highlights; softer knee avoids a hard bloom boundary halo.
    float brightness = max(hdr.r, max(hdr.g, hdr.b));
    const float threshold = 0.70;
    const float knee = 0.15;
    float weight = smoothstep(threshold - knee, threshold + knee, brightness);

    outColor = vec4(hdr * weight, 1.0);
}
