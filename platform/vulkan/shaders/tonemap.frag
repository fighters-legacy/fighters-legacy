#version 450

// Converts linear HDR to display-referred LDR using the Khronos PBR Neutral
// tonemapper, with optional bloom composite (additive, linear HDR space) and
// optional FXAA edge-smoothing (applied on the tonemapped LDR output).
// Reference tonemapper: https://github.com/KhronosGroup/ToneMapping

layout(location = 0) in  vec2 texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrBuffer;
layout(set = 0, binding = 1) uniform sampler2D bloomBuffer;
layout(set = 0, binding = 2) uniform sampler2D aoBuffer; // GTAO (AO in .r)

layout(push_constant) uniform TonemapPush {
    float texelSizeX;   // 1 / framebuffer width
    float texelSizeY;   // 1 / framebuffer height
    uint  enableFxaa;   // 1 = apply FXAA
    float bloomStrength; // 0 = no bloom
    float aoStrength;    // 0 = AO disabled
} push;

// ---------------------------------------------------------------------------
// Khronos PBR Neutral tonemapper
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Sample HDR + bloom at UV, return tonemapped LDR
// ---------------------------------------------------------------------------
vec3 sampleAndTonemap(vec2 uv) {
    vec3 hdr = texture(hdrBuffer, uv).rgb;
    // Apply ambient occlusion before bloom (forward-renderer approximation: scales total radiance).
    if (push.aoStrength > 0.0) {
        float ao = texture(aoBuffer, uv).r;
        hdr *= mix(1.0, ao, push.aoStrength);
    }
    if (push.bloomStrength > 0.0)
        hdr += texture(bloomBuffer, uv).rgb * push.bloomStrength;
    return khronosPbrNeutral(hdr);
}

// ---------------------------------------------------------------------------
// Simple FXAA — 5-tap luma edge detection + 3-sample edge-direction blur.
// Operates on the tonemapped (LDR) output for correct perceptual luma values.
// ---------------------------------------------------------------------------
vec3 applyFxaa(vec2 uv, vec2 texelSize) {
    vec3 center = sampleAndTonemap(uv);
    vec3 n = sampleAndTonemap(uv + vec2( 0.0,        -texelSize.y));
    vec3 s = sampleAndTonemap(uv + vec2( 0.0,         texelSize.y));
    vec3 e = sampleAndTonemap(uv + vec2( texelSize.x,  0.0));
    vec3 w = sampleAndTonemap(uv + vec2(-texelSize.x,  0.0));

    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaC = dot(center, luma);
    float lumaN = dot(n, luma);
    float lumaS = dot(s, luma);
    float lumaE = dot(e, luma);
    float lumaW = dot(w, luma);

    float lumaMin = min(lumaC, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaC, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    // Skip if contrast is below threshold — not an edge.
    if (lumaRange < max(0.0312, lumaMax * 0.125))
        return center;

    // Dominant edge direction.
    float edgeH = abs(lumaN - lumaS);
    float edgeV = abs(lumaE - lumaW);
    vec2 dir = edgeH > edgeV
        ? vec2(0.0, texelSize.y)
        : vec2(texelSize.x, 0.0);

    vec3 s1 = sampleAndTonemap(uv - dir);
    vec3 s2 = sampleAndTonemap(uv + dir);
    return (center + s1 + s2) / 3.0;
}

// ---------------------------------------------------------------------------
void main() {
    vec2 texelSize = vec2(push.texelSizeX, push.texelSizeY);
    vec3 ldr;
    if (push.enableFxaa != 0u)
        ldr = applyFxaa(texCoord, texelSize);
    else
        ldr = sampleAndTonemap(texCoord);

    outColor = vec4(ldr, 1.0);
}
