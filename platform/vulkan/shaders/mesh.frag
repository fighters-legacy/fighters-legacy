#version 450

// Set 0, binding 1: per-frame directional light + ambient.
layout(set = 0, binding = 1) uniform LightUBO {
    vec4 sunDirection; // xyz = world-space direction toward sun, w unused
    vec4 sunColor;     // xyz = color, w = intensity
    vec4 ambientColor; // xyz = ambient, w unused
} light;

// Set 1, binding 0: per-material base color texture.
layout(set = 1, binding = 0) uniform sampler2D baseColorTex;

layout(location = 0) in vec3 fragWorldNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec4 fragBaseColorFactor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 baseColor = texture(baseColorTex, fragUV) * fragBaseColorFactor;

    vec3 N     = normalize(fragWorldNormal);
    vec3 L     = normalize(light.sunDirection.xyz);
    float NdotL = max(dot(N, L), 0.0);

    // Lambert diffuse + ambient.  Output is linear HDR; tonemap pass handles
    // the conversion to display color space.
    vec3 diffuse = baseColor.rgb * (light.ambientColor.xyz +
                                    NdotL * light.sunColor.xyz * light.sunColor.w);
    outColor = vec4(diffuse, baseColor.a);
}
