#version 460
// 2D layer in window pixels, origin top-left (Vulkan NDC y already points down).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
layout(location = 3) in uint aTexSamp; // texture slot << 2 | sampler index
layout(push_constant) uniform P { vec2 screen; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCol;
layout(location = 2) flat out uint vTexSamp;
void main() {
    gl_Position = vec4(aPos / pc.screen * 2.0 - 1.0, 0.0, 1.0);
    vUv = aUv;
    vCol = aCol;
    vTexSamp = aTexSamp;
}
