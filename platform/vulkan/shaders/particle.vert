#version 450

// Instanced billboard vertex shader. Each instance is one particle slot read from
// the SSBO. Dead particles (age <= 0) are sent off-screen. Active particles expand
// a camera-facing quad (6 vertices, 2 triangles) in view space so they always face
// the camera regardless of orientation.

struct Particle {
    vec3  pos;       float age;
    vec3  vel;       float maxAge;
    vec4  colorStart;
    vec4  colorEnd;
    float sizeStart; float sizeEnd;
    float additive;  float _pad;
};

layout(set = 0, binding = 0) readonly buffer ParticlePool {
    Particle particles[];
};

layout(set = 0, binding = 1) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 worldOrigin; // xyz = camera world position, w unused
} camera;

// Push constant: 1 = this draw call renders additive particles, 0 = alpha.
// Particles whose blend mode doesn't match are emitted off-screen (clipped).
layout(push_constant) uniform ParticlePush {
    uint renderAdditive;
} push;

layout(location = 0) out vec4  fragColor;
layout(location = 1) out float fragAdditive;
layout(location = 2) out vec2  fragUV;

// Two-triangle quad corners in local billboard space [-0.5, 0.5].
const vec2 kCorners[6] = vec2[6](
    vec2(-0.5, -0.5), vec2( 0.5, -0.5), vec2( 0.5,  0.5),
    vec2(-0.5, -0.5), vec2( 0.5,  0.5), vec2(-0.5,  0.5)
);

const vec2 kUVs[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    Particle p = particles[gl_InstanceIndex];

    // Dead, uninitialised, or wrong blend mode for this draw — clip off-screen.
    bool wantAdditive = (push.renderAdditive != 0u);
    bool isAdditive   = (p.additive > 0.5);
    if (p.age <= 0.0 || p.maxAge <= 0.0 || isAdditive != wantAdditive) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        fragColor    = vec4(0.0);
        fragAdditive = 0.0;
        fragUV       = vec2(0.0);
        return;
    }

    // Normalised age: 0 = birth, 1 = death.
    float t = 1.0 - clamp(p.age / p.maxAge, 0.0, 1.0);

    // Interpolate colour and size by age fraction.
    vec4  color = mix(p.colorStart, p.colorEnd, t);
    float size  = mix(p.sizeStart,  p.sizeEnd,  t);

    // Camera-relative position (matches camera-relative rendering in SceneRenderer).
    vec3 camRel = p.pos - camera.worldOrigin.xyz;

    // Expand billboard corners in view space so the quad always faces the camera.
    vec4 viewPos = camera.view * vec4(camRel, 1.0);
    viewPos.xy  += kCorners[gl_VertexIndex] * size;

    gl_Position  = camera.proj * viewPos;
    fragColor    = color;
    fragAdditive = p.additive;
    fragUV       = kUVs[gl_VertexIndex];
}
