#version 460
#extension GL_EXT_nonuniform_qualifier : require
// Shadow-map caster pass: depth only; alpha-tested casters (foliage, fences) drop their cut-out texels.
layout(set = 0, binding = 0) uniform texture2D uTex[];
layout(set = 0, binding = 1) uniform sampler uSamp[5];
layout(push_constant) uniform P {
    mat4 viewProj;
    vec4 viewZ;
    vec4 fogColorScale;
    vec2 fog;
    float alphaRef;
    uint flags;
    uint tex;
    uint samp;
    float texGamma;
} pc;
layout(location = 0) in vec2 vUv;
void main() {
    if (pc.alphaRef >= 0.0 && texture(sampler2D(uTex[pc.tex], uSamp[pc.samp]), vUv).a < pc.alphaRef) discard;
}
