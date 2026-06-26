#version 450

// Sky pass — rendered into the HDR attachment after geometry with depth test
// GREATER_OR_EQUAL.  The fullscreen triangle (tonemap.vert) outputs z=0 in
// clip space, which lands at depth 0.0 (reverse-Z far plane), so this pass
// only colours pixels where no geometry was drawn.

layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 invViewProj;   // inverse of (proj * view), for ray reconstruction
    vec4 sunDirection;  // xyz = world-space direction toward sun
    vec4 sunColor;      // xyz = color, w = intensity
    vec4 skyParams;     // xyz = horizonColor, w = cloudCoverage [0=clear .. 1=full storm]
    vec4 fogParams;     // x = density, y = startDist(km), z = timeOfDay(h), w = cameraAltKm
    uint qualityMode;   // 0 = procedural, 1 = atmospheric (richer Rayleigh/Mie)
} push;

layout(location = 0) in vec2 texCoord; // from tonemap.vert: NDC xy remapped to [0,1]

layout(location = 0) out vec4 outColor;

// ---------------------------------------------------------------------------
// Procedural cloud noise — texture-free, GPU-portable.
// 4 octaves of 2D value noise (~16 hash ops per cloud pixel).
// ---------------------------------------------------------------------------
float hash(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // Hermite smoothstep
    return mix(mix(hash(i),            hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * valueNoise(p);
        p  = p * 2.1 + vec2(1.3, 1.7);
        a *= 0.5;
    }
    return v;
}

void main() {
    // Reconstruct world-space view direction from screen UV.
    // z=0 in reverse-Z NDC = far plane; the result world4.w = 0 (homogeneous direction).
    // Use xyz directly — dividing by w=0 produces NaN on the GPU.
    vec2 ndc    = texCoord * 2.0 - 1.0;
    vec4 world4 = push.invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 viewDir = normalize(world4.xyz);

    float upDot       = dot(viewDir, vec3(0.0, 1.0, 0.0));
    float cloudCoverage = push.skyParams.w;

    // Dynamic sky colors — zenith darkens under cloud cover.
    vec3 zenith  = mix(vec3(0.05, 0.10, 0.35), vec3(0.12, 0.14, 0.18), cloudCoverage);
    vec3 horizon = push.skyParams.xyz;
    // Below-horizon: atmospheric haze darkens the horizon colour rather than
    // showing a separate earthy brown. Avoids the tan band when flying low.
    vec3 ground  = horizon * 0.55;

    vec3 sky;
    if (upDot >= 0.0) {
        // pow(upDot, 0.6) gives a steeper horizon-to-zenith falloff than linear — a closer
        // approximation of Rayleigh-scattered sky brightness, brightening the lower sky band.
        sky = mix(horizon, zenith, pow(clamp(upDot, 0.0, 1.0), 0.6));
    } else {
        sky = mix(ground, horizon, clamp(1.0 + upDot * 4.0, 0.0, 1.0));
    }

    // Atmospheric quality (qualityMode == 1): add analytic Rayleigh + Mie in-scatter on top of the
    // gradient for richer colour separation — strong blue Rayleigh away from the sun, warm Mie
    // forward-scatter halo toward it, scaled by the sun being above the horizon. (A precomputed
    // transmittance/multi-scatter LUT can later replace this analytic term via this same UBO set.)
    if (push.qualityMode == 1u && upDot > -0.05) {
        float sunCos    = clamp(dot(viewDir, normalize(push.sunDirection.xyz)), -1.0, 1.0);
        float sunUp     = clamp(push.sunDirection.y, 0.0, 1.0);
        // Rayleigh phase ~ (1 + cos^2); Mie via Henyey-Greenstein (g = 0.76).
        float rayleighPh = 0.75 * (1.0 + sunCos * sunCos);
        float g = 0.76;
        float miePh = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * sunCos, 1.5);
        vec3 rayleighCol = vec3(0.18, 0.34, 0.72);
        vec3 mieCol      = push.sunColor.xyz;
        float clear      = 1.0 - cloudCoverage;
        vec3 inscatter   = (rayleighCol * rayleighPh * 0.12 + mieCol * miePh * 0.025) * sunUp * clear;
        sky += inscatter;
    }

    // Procedural cloud layer — only above the horizon.
    if (cloudCoverage > 0.01 && upDot > 0.05) {
        // Project ray onto a cloud plane at a shallow elevation.
        vec2 cloudUV = viewDir.xz / (upDot + 0.1) * 0.3;
        // Animate slowly with time of day.
        cloudUV += vec2(push.fogParams.z * 0.002, push.fogParams.z * 0.001);
        float noise = fbm(cloudUV * 3.0);
        float cloud = smoothstep(1.0 - cloudCoverage, 1.0 - cloudCoverage * 0.3, noise);

        // Cloud lighting: bright sunlit tops, darker undersides.
        float sunUp      = max(0.0, push.sunDirection.y);
        float sunAzimuth = max(0.0, dot(normalize(viewDir.xz + vec2(0.001, 0.0)),
                                        normalize(push.sunDirection.xz + vec2(0.001, 0.0))));
        vec3 cloudLit  = mix(vec3(0.55, 0.58, 0.62), vec3(1.0, 1.0, 1.0),
                             sunUp * 0.8 + sunAzimuth * 0.2);
        vec3 cloudDark = vec3(0.30, 0.32, 0.35) * (1.0 - cloudCoverage * 0.4);
        vec3 cloudColor = mix(cloudDark, cloudLit, clamp(noise * 1.5, 0.0, 1.0));
        sky = mix(sky, cloudColor, cloud);
    }

    // Baseline aerosol haze: present even in clear weather (fogParams.x == 0), concentrated near
    // the horizon (squared falloff). Weather-driven exponential fog stacks on top.
    float horizonBand = 1.0 - clamp(abs(upDot) * 5.0, 0.0, 1.0);
    float baseHaze    = horizonBand * horizonBand * 0.14;
    float fogFactor   = 1.0 - clamp(
        exp(-push.fogParams.x * max(0.0, (1.0 - abs(upDot)) * push.fogParams.y * 1000.0)),
        0.0, 1.0);
    sky = mix(sky, horizon, clamp(baseHaze + fogFactor * 0.45, 0.0, 1.0));

    // Sun disc + two-tier corona — attenuated through cloud cover.
    // smoothstep(0.9997, 1.0) ≈ 1.4° half-angle — a sharp disc rather than a wide blob.
    float sunDot   = dot(viewDir, normalize(push.sunDirection.xyz));
    float sunDisc  = smoothstep(0.9997, 1.0, sunDot) * (1.0 - cloudCoverage * 0.98);
    float sunGlow1 = pow(max(0.0, sunDot), 16.0) * 0.20 * (1.0 - cloudCoverage * 0.90); // tight corona
    float sunGlow2 = pow(max(0.0, sunDot), 3.0) * 0.015 * (1.0 - cloudCoverage * 0.70); // wide scatter
    sky += push.sunColor.xyz * push.sunColor.w * (sunDisc + sunGlow1 + sunGlow2);

    outColor = vec4(sky, 1.0);
}
