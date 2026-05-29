#version 450

// Full-screen triangle — covers [-1,1]x[-1,1] with no vertex buffer.
// gl_VertexIndex 0,1,2 generates the three vertices.
layout(location = 0) out vec2 texCoord;

void main() {
    // Positions: (-1,-1), (3,-1), (-1, 3) — triangle that covers the viewport.
    vec2 pos = vec2((gl_VertexIndex & 1) * 4 - 1,
                    (gl_VertexIndex & 2) * 2 - 1);
    texCoord = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
