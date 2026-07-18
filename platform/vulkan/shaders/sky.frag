#version 450

// Sky pass — rendered into the HDR attachment after geometry with depth test
// GREATER_OR_EQUAL.  The fullscreen triangle (tonemap.vert) outputs z=0 in
// clip space, which lands at depth 0.0 (reverse-Z far plane), so this pass
// only colours pixels where no geometry was drawn.

layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 invViewProj;     // inverse of (proj * view), for ray reconstruction
    vec4 sunDirection;    // xyz = world-space direction toward sun
    vec4 sunColor;        // xyz = color, w = intensity
    vec4 skyParams;       // xyz = horizonColor, w = cloudCoverage [0=clear .. 1=full storm]
    vec4 fogParams;       // x = density, y = startDist(km), z = timeOfDay(h), w = cameraAltKm
    vec4 moonDirection;   // xyz = world dir toward Moon, w = angular radius (#484)
    vec4 moonParams;      // x = illumination, y = nightFactor, z = celestialValid (0/1)
    mat4 worldToCelestial;// rotates a world ray into the fixed star frame (#484)
    uint qualityMode;     // 0 = procedural, 1 = atmospheric (richer Rayleigh/Mie)
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

// ---------------------------------------------------------------------------
// Night sky (#484): a procedural star field fixed on the celestial sphere, and the Moon disc with a
// geometric phase. The stars are procedural (not a real catalogue) but their ORIENTATION is real —
// they are looked up in the celestial frame (worldToCelestial), so the whole field turns about the
// pole through the night and the pole sits at an altitude equal to the observer's latitude.
// ---------------------------------------------------------------------------
vec3 hash33(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453);
}

// One star layer: cells of size 1/scale on the unit celestial sphere; `density` of them hold a star.
float starLayer(vec3 dir, float scale, float density, float size) {
    vec3 g = dir * scale;
    vec3 id = floor(g);
    vec3 rnd = hash33(id);
    if (rnd.z >= density) return 0.0;
    vec3 center = id + 0.5 + (rnd - 0.5) * 0.7;
    float d = length(g - center);
    float s = smoothstep(size, 0.0, d);
    return s * (0.35 + 0.65 * rnd.x); // brightness variety
}

// Moon disc lit by the Sun. viewDir + moonDir are world-space unit vectors; the phase falls out of
// the geometry (the lit limb faces the Sun), so no phase scalar is needed for the shape.
vec3 moonDisc(vec3 viewDir) {
    vec3 moonDir = normalize(push.moonDirection.xyz);
    float angRad = max(push.moonDirection.w, 1e-4);
    float cosSep = dot(viewDir, moonDir);
    if (cosSep < cos(angRad * 1.3)) return vec3(0.0);

    // Local disc basis at the Moon direction.
    vec3 right = cross(vec3(0.0, 1.0, 0.0), moonDir);
    if (dot(right, right) < 1e-6) right = vec3(1.0, 0.0, 0.0);
    right = normalize(right);
    vec3 up = cross(moonDir, right);

    // Disc coordinates in [-1,1] (small-angle projection).
    vec3 off = viewDir - moonDir * cosSep;
    float u = dot(off, right) / angRad;
    float v = dot(off, up) / angRad;
    float r2 = u * u + v * v;
    if (r2 > 1.0) return vec3(0.0);

    // Reconstruct the sphere surface normal at this point (near hemisphere faces the viewer).
    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 normal = normalize(right * u + up * v - moonDir * z);
    float lambert = max(0.0, dot(normal, normalize(push.sunDirection.xyz)));

    vec3 albedo = vec3(0.86, 0.86, 0.82);
    float lit = pow(lambert, 0.85);
    float edge = smoothstep(1.0, 0.92, r2); // soft limb
    // Faint earthshine so the unlit disc is a dim circle, not a hole.
    return albedo * (lit + 0.015) * edge;
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

    // Night darkening (#484): fade the daylit sky toward a dark night sky as the sun sets, so stars
    // and the Moon have a dark backdrop. Above ~3.5° sun elevation this is a no-op (dayFactor == 1).
    float dayFactor = smoothstep(-0.12, 0.06, push.sunDirection.y);
    sky *= mix(0.02, 1.0, dayFactor);

    // Sun disc + two-tier corona — attenuated through cloud cover.
    // smoothstep(0.9997, 1.0) ≈ 1.4° half-angle — a sharp disc rather than a wide blob.
    float sunDot   = dot(viewDir, normalize(push.sunDirection.xyz));
    float sunDisc  = smoothstep(0.9997, 1.0, sunDot) * (1.0 - cloudCoverage * 0.98);
    float sunGlow1 = pow(max(0.0, sunDot), 16.0) * 0.20 * (1.0 - cloudCoverage * 0.90); // tight corona
    float sunGlow2 = pow(max(0.0, sunDot), 3.0) * 0.015 * (1.0 - cloudCoverage * 0.70); // wide scatter
    sky += push.sunColor.xyz * push.sunColor.w * (sunDisc + sunGlow1 + sunGlow2);

    // Stars + Moon (#484) — only when the celestial frame is valid and the sun is low. Stars fade
    // with cloud cover and near/below the horizon; the Moon is drawn at its true position/phase.
    float nightFactor  = push.moonParams.y;
    if (push.moonParams.z > 0.5 && nightFactor > 0.001) {
        float clear = 1.0 - cloudCoverage;
        if (upDot > -0.05) {
            vec3 celDir = mat3(push.worldToCelestial) * viewDir;
            float stars = starLayer(celDir, 130.0, 0.07, 0.55) * 0.8
                        + starLayer(celDir, 210.0, 0.05, 0.45) * 0.6
                        + starLayer(celDir, 320.0, 0.03, 0.40) * 0.4;
            stars *= clear * nightFactor * smoothstep(-0.02, 0.14, upDot);
            sky += vec3(stars) * vec3(0.9, 0.92, 1.0);
        }
        // The Moon disc is added regardless of horizon test (it may sit low); its own disc math clips.
        sky += moonDisc(viewDir) * (0.35 + 0.65 * nightFactor) * (1.0 - cloudCoverage * 0.9);
    }

    outColor = vec4(sky, 1.0);
}
