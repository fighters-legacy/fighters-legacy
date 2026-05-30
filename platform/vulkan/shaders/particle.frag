#version 450

layout(location = 0) in vec4  fragColor;
layout(location = 1) in float fragAdditive;
layout(location = 2) in vec2  fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    // Circular soft-edge mask in UV space (UV [0,1] → NDC [-1,1]).
    vec2  uv   = fragUV * 2.0 - 1.0;
    float dist = dot(uv, uv);
    if (dist > 1.0)
        discard;

    // Smooth quadratic falloff toward the edge.
    float alpha = (1.0 - dist) * (1.0 - dist);

    vec4 col = fragColor;
    col.a   *= alpha;

    // For additive blending the alpha channel controls how much light is added;
    // for alpha blending it controls transparency. The pipeline blend state
    // is set per-pipeline at create time; the shader outputs the same value.
    outColor = col;
}
