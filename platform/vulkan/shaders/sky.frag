#version 450

// Sky pass — rendered into the HDR attachment after geometry with depth test
// GREATER_OR_EQUAL.  The fullscreen triangle (tonemap.vert) outputs z=0 in
// clip space, which lands at depth 0.0 (reverse-Z far plane), so this pass
// only colours pixels where no geometry was drawn.

layout(push_constant) uniform SkyPC {
    mat4 invViewProj;   // inverse of (proj * view), for ray reconstruction
    vec4 sunDirection;  // xyz = world-space direction toward sun
    vec4 sunColor;      // xyz = color, w = intensity
} push;

layout(location = 0) in vec2 texCoord; // from tonemap.vert: NDC xy remapped to [0,1]

layout(location = 0) out vec4 outColor;

void main() {
    // Reconstruct world-space view direction from screen UV.
    vec2 ndc    = texCoord * 2.0 - 1.0;
    vec4 world4 = push.invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 viewDir = normalize(world4.xyz / world4.w);

    // Vertical gradient: zenith (up) to horizon to ground haze.
    float upDot = dot(viewDir, vec3(0.0, 1.0, 0.0));
    vec3 zenith  = vec3(0.05, 0.10, 0.35);
    vec3 horizon = vec3(0.40, 0.55, 0.75);
    vec3 ground  = vec3(0.30, 0.28, 0.25);

    vec3 sky;
    if (upDot >= 0.0) {
        sky = mix(horizon, zenith, clamp(upDot, 0.0, 1.0));
    } else {
        sky = mix(ground, horizon, clamp(1.0 + upDot * 4.0, 0.0, 1.0));
    }

    // Sun disc.
    float sunDot  = dot(viewDir, normalize(push.sunDirection.xyz));
    float sunDisc = smoothstep(0.9995, 0.9999, sunDot);
    sky += push.sunColor.xyz * push.sunColor.w * sunDisc;

    outColor = vec4(sky, 1.0);
}
