#version 460
// Shadow-map caster pass: pc.viewProj = one cascade's light view-projection. Push block as scene.vert.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
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
layout(location = 0) out vec2 vUv;
void main() {
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
    vUv = aUv;
}
