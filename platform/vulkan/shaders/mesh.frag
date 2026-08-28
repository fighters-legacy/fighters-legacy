#version 450

const float PI = 3.14159265358979;

// ── Set 0: per-frame data ────────────────────────────────────────────────────

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 worldOrigin;  // xyz = camera world position
    vec4 planetCenter; // xyz = planet centre, CAMERA-RELATIVE (CPU double-rebased), #475 radial up
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

// Terrain biome arrays (#446): set 2, layer index = biome id (0 grass, 1 dirt, 2 rock, 3 snow).
layout(set = 2, binding = 0) uniform sampler2DArray biomeColorArray;
layout(set = 2, binding = 1) uniform sampler2DArray biomeNormalOrmArray; // RG=normal.xy B=roughness A=occlusion

// ── Push constants ───────────────────────────────────────────────────────────

layout(push_constant) uniform PushConstants {
    mat4  model;
    vec4  baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float shadingMode; // 0 = PBR, 1 = terrain biomes, 2 = debug face colour, 3 = runway, 4 = terrain satellite (#488)
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

// Biome weights (x grass, y dirt, z rock, w snow — biome array layer order) from an ESA WorldCover
// class. MIRROR of engine/render/BiomeWeights.h biomeWeightsForWorldCover — keep in sync (pinned by
// test_biome_weights). 255 / unmapped returns all-zero: the caller uses the elevation/slope fallback.
vec4 biomeWeightsForWorldCover(int cls) {
    if (cls == 10 || cls == 20 || cls == 30 || cls == 40 || cls == 100) return vec4(1.0, 0.0, 0.0, 0.0);
    if (cls == 90 || cls == 95) return vec4(0.7, 0.3, 0.0, 0.0);
    if (cls == 50) return vec4(0.0, 0.6, 0.4, 0.0);
    if (cls == 60) return vec4(0.0, 0.45, 0.55, 0.0);
    if (cls == 70) return vec4(0.0, 0.0, 0.0, 1.0);
    if (cls == 80) return vec4(0.0, 0.5, 0.5, 0.0);
    return vec4(0.0);
}
float worldCoverWaterness(int cls) {
    if (cls == 80) return 1.0;
    if (cls == 90 || cls == 95) return 0.5;
    return 0.0;
}

// Elevation/slope fallback (pre-#475) — used for tiles with no land-cover layer (class 255).
// normElev is (h + 1000) / 10000 (the packed value), so elevationM = normElev*10000 - 1000.
vec4 biomeWeightsElevSlope(float normElev, float slopeFlat) {
    float elevationM = normElev * 10000.0 - 1000.0;
    float grass = smoothstep(450.0, 300.0, elevationM) * smoothstep(0.65, 0.85, slopeFlat);
    float snow  = smoothstep(700.0, 820.0, elevationM);
    float rock  = (1.0 - smoothstep(0.55, 0.80, slopeFlat)) + smoothstep(520.0, 660.0, elevationM) * (1.0 - snow);
    float dirt  = max(0.0, 1.0 - grass - rock - snow);
    return vec4(grass, dirt, rock, snow);
}

// Biome-blended terrain albedo sampled from the biome texture ARRAYS (#446), weighted by `w`.
// detailUV is the spherical-valid per-vertex detail coordinate in metres (#475) — seamless across
// tiles and LOD levels, so the tiling no longer relies on the float-cancelling absolute world XZ.
vec3 sampleBiomeAlbedo(vec4 w, vec2 detailUV) {
    vec2 fineUV = detailUV / 4.0;    // 4 m fine tiling
    vec2 coarseUV = detailUV / 30.0; // 30 m coarse tiling
    vec3 col = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec3 c = mix(texture(biomeColorArray, vec3(fineUV, float(i))).rgb,
                     texture(biomeColorArray, vec3(coarseUV, float(i))).rgb, 0.35);
        col += c * w[i];
    }
    return col;
}

// ── Vertex inputs ────────────────────────────────────────────────────────────

layout(location = 0) in vec3  fragWorldPos;
layout(location = 1) in vec3  fragWorldNormal;
layout(location = 2) in vec3  fragWorldTangent;
layout(location = 3) in float fragTangentHandedness;
layout(location = 4) in vec2  fragUV;
// Packed terrain data (#475): .x = WorldCover class (255 = none), .y = normalized elevation,
// .zw = spherical-valid detail coordinate in metres. For non-terrain meshes this is the real tangent.
layout(location = 5) in vec4  fragRawTangent;

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

// Procedural runway markings (#487) from the slab's runway-local UV (u = along the runway [0,1],
// v = across [0,1]). Overlays white centerline dashes, edge lines, and threshold "piano keys" onto
// the paved base colour — no texture, no content dependency.
vec3 runwayMarkings(vec2 uv, vec3 base) {
    const vec3 paint = vec3(0.85);
    float mark = 0.0;
    // Edge lines: a thin white stripe just inside each long edge.
    if ((uv.y > 0.03 && uv.y < 0.06) || (uv.y > 0.94 && uv.y < 0.97)) mark = 1.0;
    // Centerline: dashed down the middle, away from the thresholds.
    if (abs(uv.y - 0.5) < 0.015 && uv.x > 0.10 && uv.x < 0.90 && fract(uv.x * 40.0) < 0.6) mark = 1.0;
    // Threshold "piano keys": longitudinal bars across each end.
    if ((uv.x < 0.05 || uv.x > 0.95) && fract(uv.y * 8.0) < 0.6) mark = 1.0;
    return mix(base, paint, mark);
}

void main() {
    // Base color
    vec4 baseColor = texture(baseColorTex, fragUV) * push.baseColorFactor;

    // Debug face colour (builtin placeholder) takes priority; otherwise terrain (land-cover biomes,
    // #475), or runway markings (#487). Terrain/debug/runway use the geometric (flat) normal.
    bool isTerrain   = (push.shadingMode > 0.5 && push.shadingMode < 1.5);
    bool isRunway    = (push.shadingMode > 2.5 && push.shadingMode < 3.5);
    bool isSatellite = (push.shadingMode > 3.5 && push.shadingMode < 4.5); // #488: albedo from the satellite texture
    bool isNormals   = (push.shadingMode > 4.5); // #838 authoring view: visualize the final world-space normal

    // Packed terrain data (#475): the spherical-valid detail coordinate replaces the old
    // fragWorldPos.xz + worldOrigin.xz, which cancelled to metre-precision garbage at Earth radius.
    int   terrainClass   = int(fragRawTangent.x + 0.5);
    float terrainElev    = fragRawTangent.y;   // normalized (h + 1000) / 10000
    vec2  terrainDetail  = fragRawTangent.zw;  // metres, seamless across tiles/LOD
    if (push.shadingMode > 1.5 && push.shadingMode < 2.5) {
        baseColor.rgb = faceColor(normalize(fragWorldNormal));
    } else if (isTerrain) {
        // Slope from the RADIAL up (planet-centre relative) — correct anywhere on the sphere.
        vec3 radialUp = normalize(fragWorldPos - camera.planetCenter.xyz);
        float slopeFlat = clamp(dot(normalize(fragWorldNormal), radialUp), 0.0, 1.0);
        vec4 w;
        if (terrainClass >= 254) {
            w = biomeWeightsElevSlope(terrainElev, slopeFlat); // no land-cover -> fallback
        } else {
            w = biomeWeightsForWorldCover(terrainClass);
            // Steep ground reads as rock regardless of land class; high ground gets snow-capped.
            float steep = 1.0 - smoothstep(0.55, 0.80, slopeFlat);
            w = mix(w, vec4(0.0, 0.0, 1.0, 0.0), steep * 0.6 * (1.0 - worldCoverWaterness(terrainClass)));
            float snow = smoothstep(0.26, 0.32, terrainElev) * (1.0 - worldCoverWaterness(terrainClass));
            w = mix(w, vec4(0.0, 0.0, 0.0, 1.0), snow);
        }
        w /= (dot(w, vec4(1.0)) + 1e-4);
        vec3 albedo = sampleBiomeAlbedo(w, terrainDetail);
        float water = worldCoverWaterness(terrainClass);
        baseColor.rgb = mix(albedo, vec3(0.015, 0.045, 0.07), water); // open water: dark
    } else if (isRunway) {
        baseColor.rgb = runwayMarkings(fragUV, baseColor.rgb);
    }

    // ORM: R=occlusion, G=roughness, B=metallic
    vec3  orm       = texture(ormTex, fragUV).rgb;
    float occlusion = orm.r;
    float roughness = clamp(orm.g * push.roughnessFactor, 0.04, 1.0);
    float metallic  = clamp(orm.b * push.metallicFactor,  0.0,  1.0);

    // Normal mapping — tangent space → world space via TBN.
    //
    // TERRAIN AND SATELLITE TILES ARE EXCLUDED, and that is load-bearing rather than an optimisation
    // (#1360). Those meshes REPURPOSE the tangent attribute to carry terrain data (#475):
    // `fragRawTangent` is .x = land-cover class, .y = normalized elevation, .zw = the detail
    // coordinate IN METRES. So `fragWorldTangent` is not a tangent and `fragTangentHandedness` is not
    // +/-1 — it is a metre value in the thousands. `B` came out ~1000x oversized, and since the flat
    // normal map samples (0.48, 0.48, 1.0) rather than exactly (0.5, 0.5, 1.0), that residual -0.03
    // in x/y was multiplied by a basis vector of length 1000 and swamped the geometric normal ~30:1.
    // The shading normal ended up roughly perpendicular to the ground: measured dot(geoN, L) = 1.00
    // at solar noon while NdotL = 0.00, so terrain took ambient light only and rendered near-black,
    // banded in tile-sized stripes as the detail coordinate ramped and reset across each tile.
    //
    // A terrain tile has no normal map of its own anyway — its micro-surface is the detail-noise
    // perturbation below — so the geometric normal IS the right answer here, as the shading-mode
    // comment above already said.
    vec3 N = normalize(fragWorldNormal);
    if (!isTerrain && !isSatellite) {
        vec3 T = normalize(fragWorldTangent);
        T = normalize(T - dot(T, N) * N); // Gram-Schmidt re-orthogonalise
        vec3 B = cross(N, T) * fragTangentHandedness;
        mat3 TBN = mat3(T, B, N);

        vec3 normalSample = texture(normalTex, fragUV).rgb * 2.0 - 1.0;
        N = normalize(TBN * normalSample);
    }

    // Terrain micro-surface: perturb the normal with finite-differenced detail noise, and roughen
    // slightly with the same field, fading out with distance to avoid shimmer. Keyed on the
    // spherical-valid detail coordinate (#475), not the float-cancelling absolute world XZ. Open
    // water is left smooth + glossy instead (a specular sheen, no bump).
    if (isTerrain || isSatellite) {
        // Satellite imagery already carries visual detail, so use only a gentle bump there.
        float water = isSatellite ? 0.0 : worldCoverWaterness(terrainClass);
        float detailScale = isSatellite ? 0.4 : 1.0;
        float detailFade = (1.0 - smoothstep(150.0, 900.0, length(fragWorldPos))) * (1.0 - water) * detailScale;
        if (detailFade > 0.001) {
            const float eps = 0.5; // metres
            float h0 = noiseT(terrainDetail / 6.0);
            float hx = noiseT((terrainDetail + vec2(eps, 0.0)) / 6.0) - h0;
            float hz = noiseT((terrainDetail + vec2(0.0, eps)) / 6.0) - h0;
            N = normalize(N + vec3(hx, 0.0, hz) * 2.2 * detailFade);
            roughness = clamp(roughness + (h0 - 0.5) * 0.15 * detailFade, 0.5, 1.0);
        }
        roughness = mix(roughness, 0.12, water); // glossy water
    }

    // Normals debug view (#838): visualize the FINAL shaded normal (post normal-map) as RGB, so an
    // author can debug a normal map without flying the jet. It still passes through the sky/tonemap
    // like the face-color view; hue direction is what matters. N is fully resolved by here.
    if (isNormals) {
        outColor = vec4(N * 0.5 + 0.5, 1.0);
        outNormal = vec4(octEncode(N), 0.0, 1.0);
        return;
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
