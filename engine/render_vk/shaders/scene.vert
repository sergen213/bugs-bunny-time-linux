#version 460
// Mirrors kVs3D of the GL renderer. Push block layout is shared with scene.frag and renderer.cpp (Push3D).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
layout(push_constant) uniform P {
    mat4 viewProj;      // Vulkan clip space (z 0..1); y is flipped by a negative viewport height
    vec4 viewZ;         // third row of the view matrix: view-space z = dot(viewZ, p)
    vec4 fogColorScale; // rgb fog colour, a = colour scale
    vec2 fog;
    float alphaRef;
    uint flags;
    uint tex;
    uint samp;
    float texGamma;
} pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCol;
layout(location = 2) out float vDist;
void main() {
    vec4 p = vec4(aPos, 1.0);
    gl_Position = pc.viewProj * p;
    vUv = aUv;
    vCol = aCol;
    vDist = -dot(pc.viewZ, p);
}
