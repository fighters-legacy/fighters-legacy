#version 450

// Converts linear HDR to display-referred LDR using the Khronos PBR Neutral
// tonemapper.  Reference: https://github.com/KhronosGroup/ToneMapping
layout(location = 0) in  vec2 texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;

vec3 khronosPbrNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation     = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d      = 1.0 - startCompression;
    float       newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

void main() {
    vec3 hdr = texture(hdrBuffer, texCoord).rgb;
    outColor = vec4(khronosPbrNeutral(hdr), 1.0);
}
