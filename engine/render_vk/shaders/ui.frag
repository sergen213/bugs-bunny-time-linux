#version 460
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D uTex[];
layout(set = 0, binding = 1) uniform sampler uSamp[5];
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCol;
layout(location = 2) flat in uint vTexSamp;
layout(location = 0) out vec4 oCol;
void main() {
    oCol = texture(sampler2D(uTex[nonuniformEXT(vTexSamp >> 2)], uSamp[nonuniformEXT(vTexSamp & 3u)]), vUv) * vCol;
}
