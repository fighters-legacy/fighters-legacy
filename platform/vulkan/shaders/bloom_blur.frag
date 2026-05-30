#version 450

// Separable 9-tap Gaussian blur for bloom.
// push.isVertical == 0: horizontal pass (bloom → bloomAux)
// push.isVertical != 0: vertical   pass (bloomAux → bloom)
// Weights normalise to ~1.0 (0.227 + 2×(0.194+0.121+0.054+0.016) = 0.997).

layout(location = 0) in  vec2 texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputBuffer;

layout(push_constant) uniform BloomPush {
    float texelSizeX;
    float texelSizeY;
    uint  isVertical;
    float _pad;
} push;

void main() {
    const float weights[5] = float[](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);

    vec2 step = push.isVertical != 0u
        ? vec2(0.0, push.texelSizeY)
        : vec2(push.texelSizeX, 0.0);

    vec3 result = texture(inputBuffer, texCoord).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = float(i) * step;
        result += texture(inputBuffer, texCoord + off).rgb * weights[i];
        result += texture(inputBuffer, texCoord - off).rgb * weights[i];
    }

    outColor = vec4(result, 1.0);
}
