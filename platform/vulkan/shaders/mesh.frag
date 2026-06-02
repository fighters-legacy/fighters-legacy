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
    vec4 splitDepths;      // x/y/z = view-space end of cascades 0/1/2
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
} push;

// ── Vertex inputs ────────────────────────────────────────────────────────────

layout(location = 0) in vec3  fragWorldPos;
layout(location = 1) in vec3  fragWorldNormal;
layout(location = 2) in vec3  fragWorldTangent;
layout(location = 3) in float fragTangentHandedness;
layout(location = 4) in vec2  fragUV;

layout(location = 0) out vec4 outColor;

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
    // Select cascade based on view-space distance.
    uint cascade;
    if      (viewDepth < shadow.splitDepths.x) cascade = 0u;
    else if (viewDepth < shadow.splitDepths.y) cascade = 1u;
    else if (viewDepth < shadow.splitDepths.z) cascade = 2u;
    else                                        cascade = 3u;

    // lightViewProj was built for absolute world space.
    vec3 absPos   = camRelWorldPos + camera.worldOrigin.xyz;
    vec4 lightClip = shadow.lightViewProj[cascade] * vec4(absPos, 1.0);
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

    // Exponential fog in camera-relative view space.
    // fogParams.x = density coefficient; fogParams.y = start distance (metres).
    // When density is 0 (clear weather) the exp returns 1.0 and fogAmount = 0.
    float viewDist  = length(fragWorldPos);
    float fogAmount = 1.0 - clamp(
        exp(-light.fogParams.x * max(0.0, viewDist - light.fogParams.y)),
        0.0, 1.0);
    vec3 fogColor = light.ambientColor.xyz * 2.5;
    outColor = vec4(mix(litColor, fogColor, fogAmount), baseColor.a);
}
