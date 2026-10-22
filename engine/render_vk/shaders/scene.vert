#version 460
// Mirrors kVs3D of the GL renderer. Push block layout is shared with scene.frag and renderer.cpp (Push3D).
// LIT (Enhanced lighting): also passes the world position and the smooth vertex normal to the lit scene.frag
// variants. MV (FSR): also the unjittered current and previous clip positions for the motion-vector attachment.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
#ifdef LIT
layout(location = 3) in vec4 aNormal; // R8G8B8A8_SNORM, xyz
#endif
#ifdef MV
layout(location = 4) in vec3 aPrev;   // this vertex's position in the previous frame
#endif
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
#ifdef LIT
layout(location = 3) out vec3 vWorld;
layout(location = 4) out vec3 vNormal;
#endif
#ifdef MV
// renderer.cpp LightUbo (only the two matrices at its end are read here)
layout(set = 1, binding = 0) uniform Light {
    mat4 cascade[4];
    vec4 splits, texel, sun, eye, rt;
    uint mode;
    float rayMin;
    uint reflect;
    uint temporal;
    mat4 curViewProj;  // this frame, without the FSR jitter
    mat4 prevViewProj; // previous frame, without jitter
} L;
layout(location = 5) out vec4 vCur;
layout(location = 6) out vec4 vPrev;
#endif
void main() {
    vec4 p = vec4(aPos, 1.0);
    gl_Position = pc.viewProj * p;
    vUv = aUv;
    vCol = aCol;
    vDist = -dot(pc.viewZ, p);
#ifdef LIT
    vWorld = aPos;
    vNormal = aNormal.xyz;
#endif
#ifdef MV
    vCur = L.curViewProj * p;
    vPrev = L.prevViewProj * vec4(aPrev, 1.0);
#endif
}
