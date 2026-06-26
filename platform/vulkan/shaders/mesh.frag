#version 450

const float PI = 3.14159265358979;

// ── Set 0: per-frame data ────────────────────────────────────────────────────

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 worldOrigin; // xyz = camera world position
} camera;

layout(set = 0, binding = 1) uniform LightUBO {
    vec4 sunDirection; // xyz = world-space direction toward sun
    vec4 sunColor;     // xyz = color, w = intensity
    vec4 ambientColor; // xyz = ambient, w unused
    vec4 fogParams;    // x = density, y = startDist(m), z = timeOfDay(h), w = unused
} light;

layout(set = 0, binding = 2) uniform ShadowUBO {
    mat4 lightViewProj[4]; // one per cascade (absolute world space)
    vec4 splitDepths;      // x/y/z = view-space end of cascades 0/1/2, w = shadow far
    uint numCascades;      // active cascade count; 0 = shadows disabled
} shadow;

layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadowMap;

// ── Set 1: per-material textures ─────────────────────────────────────────────

layout(set = 1, binding = 0) uniform sampler2D baseColorTex;
layout(set = 1, binding = 1) uniform sampler2D normalTex;  // tangent-space normal map
layout(set = 1, binding = 2) uniform sampler2D ormTex;     // R=occlusion G=roughness B=metallic

// ── Push constants ───────────────────────────────────────────────────────────

layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float shadingMode; // 0 = normal PBR, 1 = terrain elevation/slope, 2 = debug face colour
} push;

// Debug per-face colour for the builtin placeholder wedge, keyed off the (flat) face normal so
// each face reads as a distinct colour and the orientation is unambiguous: bottom (-Y) = red,
// back (-X) = green, right (+Z) = blue, left (-Z) = yellow.
vec3 faceColor(vec3 n) {
    if (n.y < -0.5) return vec3(0.85, 0.10, 0.10); // bottom
    if (n.x < -0.5) return vec3(0.10, 0.70, 0.15); // back
    if (n.z > 0.0)  return vec3(0.15, 0.35, 0.90); // right (+Z)
    return vec3(0.90, 0.80, 0.10);                 // left (-Z)
}

// Procedural value noise (world-space XZ) for terrain micro-detail — same hash family as the
// sky cloud noise, kept self-contained here.
float hashT(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}
float noiseT(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hashT(i), hashT(i + vec2(1, 0)), f.x), mix(hashT(i + vec2(0, 1)), hashT(i + vec2(1, 1)), f.x), f.y);
}

// 4-biome weights from elevation + slope flatness (grass, dirt, rock, snow), normalised.
vec4 biomeWeights(float elevationM, float slopeFlat) {
    float grass = smoothstep(450.0, 300.0, elevationM) * smoothstep(0.65, 0.85, slopeFlat);
    float snow  = smoothstep(700.0, 820.0, elevationM);
    float rock  = (1.0 - smoothstep(0.55, 0.80, slopeFlat)) + smoothstep(520.0, 660.0, elevationM) * (1.0 - snow);
    float dirt  = max(0.0, 1.0 - grass - rock - snow);
    vec4 w = vec4(grass, dirt, rock, snow);
    return w / (dot(w, vec4(1.0)) + 1e-4);
}

// Biome-blended terrain albedo with world-space detail-noise variation. worldXZ is absolute world
// metres so detail tiles seamlessly across chunk boundaries.
vec3 terrainAlbedo(float elevationM, vec3 geoNormal, vec2 worldXZ) {
    const vec3 grassCol = vec3(0.23, 0.42, 0.15);
    const vec3 dirtCol  = vec3(0.46, 0.37, 0.22);
    const vec3 rockCol  = vec3(0.42, 0.40, 0.38);
    const vec3 snowCol  = vec3(0.90, 0.92, 0.95);
    vec4 w = biomeWeights(elevationM, clamp(geoNormal.y, 0.0, 1.0));
    vec3 col = grassCol * w.x + dirtCol * w.y + rockCol * w.z + snowCol * w.w;
    // Detail: blend coarse (30 m) and fine (6 m) noise to break up flat shading.
    float d = noiseT(worldXZ / 30.0) * 0.7 + noiseT(worldXZ / 6.0) * 0.3;
    col *= 0.8 + 0.4 * d;
    return col;
}

// ── Vertex inputs ────────────────────────────────────────────────────────────

layout(location = 0) in vec3  fragWorldPos;
layout(location = 1) in vec3  fragWorldNormal;
layout(location = 2) in vec3  fragWorldTangent;
layout(location = 3) in float fragTangentHandedness;
layout(location = 4) in vec2  fragUV;

layout(location = 0) out vec4 outColor;
// G-buffer world-space normal (octahedral-encoded into RG). Written only when the forward-opaque
// pass binds a second colour attachment; ignored by single-attachment passes (transparent/sky).
layout(location = 1) out vec4 outNormal;

// Octahedral normal encoding (Cigolle et al.) → [0,1]² for an RG16F target.
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = (n.z < 0.0) ? (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0) : n.xy;
    return e * 0.5 + 0.5;
}

// ── PBR helpers ──────────────────────────────────────────────────────────────

float DistributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float GeometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    return GeometrySchlickGGX(NdotV, roughness) *
           GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ── Shadow sampling ──────────────────────────────────────────────────────────

float sampleShadow(vec3 camRelWorldPos, float viewDepth) {
    if (shadow.numCascades == 0u)
        return 1.0;

    // Select cascade based on view-space distance.
    uint cascade;
    if      (viewDepth < shadow.splitDepths.x) cascade = 0u;
    else if (viewDepth < shadow.splitDepths.y) cascade = 1u;
    else if (viewDepth < shadow.splitDepths.z) cascade = 2u;
    else                                        cascade = 3u;
    // Clamp to the number of active cascades; unused split entries are set to
    // kShadowFar so they never trigger the above branches when numCascades < 4.
    cascade = min(cascade, shadow.numCascades - 1u);

    // lightViewProj is built in camera-relative space (same rebase as the geometry), so the
    // camera-relative fragment position is fed directly — no worldOrigin reconstruction.
    vec4 lightClip = shadow.lightViewProj[cascade] * vec4(camRelWorldPos, 1.0);
    vec3 sc        = lightClip.xyz / lightClip.w;

    // NDC [-1,1] xy → UV [0,1]; Z is already [0,1] (forward-Z shadow map).
    vec2 uv = sc.xy * 0.5 + 0.5;

    // Clamp to avoid bleeding at cascade edges.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;

    float refDepth = clamp(sc.z - 0.001, 0.0, 1.0); // small bias
    return texture(shadowMap, vec4(uv, float(cascade), refDepth));
}

// ── Main ─────────────────────────────────────────────────────────────────────

void main() {
    // Base color
    vec4 baseColor = texture(baseColorTex, fragUV) * push.baseColorFactor;

    // Debug face colour (builtin placeholder) takes priority; otherwise terrain elevation/slope
    // shading. fragWorldPos is camera-relative, so add the camera world Y to recover absolute
    // elevation. Both use the geometric (flat) normal.
    bool isTerrain = (push.shadingMode > 0.5 && push.shadingMode < 1.5);
    vec2 terrainXZ = fragWorldPos.xz + camera.worldOrigin.xz; // absolute world XZ (seamless tiling)
    if (push.shadingMode > 1.5) {
        baseColor.rgb = faceColor(normalize(fragWorldNormal));
    } else if (isTerrain) {
        float elevationM = fragWorldPos.y + camera.worldOrigin.y;
        baseColor.rgb = terrainAlbedo(elevationM, normalize(fragWorldNormal), terrainXZ);
    }

    // ORM: R=occlusion, G=roughness, B=metallic
    vec3  orm       = texture(ormTex, fragUV).rgb;
    float occlusion = orm.r;
    float roughness = clamp(orm.g * push.roughnessFactor, 0.04, 1.0);
    float metallic  = clamp(orm.b * push.metallicFactor,  0.0,  1.0);

    // Normal mapping — tangent space → world space via TBN
    vec3 N = normalize(fragWorldNormal);
    vec3 T = normalize(fragWorldTangent);
    T = normalize(T - dot(T, N) * N); // Gram-Schmidt re-orthogonalise
    vec3 B = cross(N, T) * fragTangentHandedness;
    mat3 TBN = mat3(T, B, N);

    vec3 normalSample = texture(normalTex, fragUV).rgb * 2.0 - 1.0;
    N = normalize(TBN * normalSample);

    // Terrain micro-surface: perturb the normal with finite-differenced detail noise (world XZ),
    // and roughen slightly with the same field, fading out with distance to avoid shimmer.
    if (isTerrain) {
        float detailFade = 1.0 - smoothstep(150.0, 900.0, length(fragWorldPos));
        if (detailFade > 0.001) {
            const float eps = 0.5; // metres
            float h0 = noiseT(terrainXZ / 6.0);
            float hx = noiseT((terrainXZ + vec2(eps, 0.0)) / 6.0) - h0;
            float hz = noiseT((terrainXZ + vec2(0.0, eps)) / 6.0) - h0;
            N = normalize(N + vec3(hx, 0.0, hz) * 2.2 * detailFade);
            roughness = clamp(roughness + (h0 - 0.5) * 0.15 * detailFade, 0.5, 1.0);
        }
    }

    // View and half vectors (camera-relative: camera is at origin)
    vec3 V = normalize(-fragWorldPos);
    vec3 L = normalize(light.sunDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // PBR material
    vec3 albedo = baseColor.rgb;
    vec3 F0     = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance BRDF
    float D  = DistributionGGX(NdotH, roughness);
    float G  = GeometrySmith(NdotV, NdotL, roughness);
    vec3  F  = FresnelSchlick(VdotH, F0);

    vec3 kS      = F;
    vec3 kD      = (1.0 - kS) * (1.0 - metallic);
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);
    vec3 diffuse  = kD * albedo / PI;

    // Shadow
    float viewDepth   = -(camera.view * vec4(fragWorldPos, 1.0)).z;
    float shadowFactor = sampleShadow(fragWorldPos, viewDepth);

    // Direct sun + ambient
    vec3 sunRadiance = light.sunColor.xyz * light.sunColor.w;
    vec3 direct  = (diffuse + specular) * NdotL * sunRadiance * shadowFactor;
    vec3 ambient = albedo * occlusion * light.ambientColor.xyz;

    vec3 litColor = ambient + direct;

    // Distance attenuation. fogParams.w selects the model:
    //   0 → exponential weather fog (procedural sky quality)
    //   1 → analytic aerial perspective (atmospheric sky quality): distant geometry is extinguished
    //       and replaced by Rayleigh (blue) + Mie (sun-tinted) in-scatter, the signature flight-sim
    //       vista look. Reuses the same scattering form as the atmospheric sky shader.
    float viewDist = length(fragWorldPos);
    vec3 outRgb;
    if (light.fogParams.w >= 0.5) {
        vec3 viewDir = (viewDist > 1e-4) ? (fragWorldPos / viewDist) : vec3(0.0, 0.0, 1.0);
        float sunCos = clamp(dot(viewDir, normalize(light.sunDirection.xyz)), -1.0, 1.0);
        float sunUp = clamp(light.sunDirection.y, 0.0, 1.0);
        // Extinction grows with distance (scale height ~40 km of equivalent optical depth).
        float t = 1.0 - exp(-viewDist * 2.2e-5);
        float rayleighPh = 0.75 * (1.0 + sunCos * sunCos);
        float g = 0.76;
        float miePh = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * sunCos, 1.5);
        vec3 rayleighCol = vec3(0.18, 0.34, 0.72);
        vec3 inscatter = (rayleighCol * rayleighPh + light.sunColor.xyz * miePh * 0.08) * (0.6 + 0.4 * sunUp);
        outRgb = mix(litColor, inscatter, t);
        // Add the weather fog density on top so storms still occlude even in atmospheric mode.
        float fogAmount = 1.0 - clamp(exp(-light.fogParams.x * max(0.0, viewDist - light.fogParams.y)), 0.0, 1.0);
        outRgb = mix(outRgb, light.ambientColor.xyz * 2.5, fogAmount);
    } else {
        // Exponential fog. When density is 0 (clear weather) the exp returns 1.0 and fogAmount = 0.
        float fogAmount = 1.0 - clamp(exp(-light.fogParams.x * max(0.0, viewDist - light.fogParams.y)), 0.0, 1.0);
        outRgb = mix(litColor, light.ambientColor.xyz * 2.5, fogAmount);
    }
    outColor = vec4(outRgb, baseColor.a);

    // G-buffer normal (world space, normal-mapped). Consumed by GTAO; dropped by passes that bind
    // only the HDR attachment.
    outNormal = vec4(octEncode(N), 0.0, 1.0);
}
