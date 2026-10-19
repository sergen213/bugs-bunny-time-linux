#version 460
#extension GL_EXT_nonuniform_qualifier : require
// Mirrors kFs3D of the GL renderer.
layout(set = 0, binding = 0) uniform texture2D uTex[];
layout(set = 0, binding = 1) uniform sampler uSamp[4];
layout(push_constant) uniform P {
    mat4 viewProj;
    vec4 viewZ;
    vec4 fogColorScale;
    vec2 fog;       // linear fog start, end; end <= start = off
    float alphaRef; // texels below are discarded (AlphaTest); negative = off
    uint flags;     // bit 0: additive/subtractive surface, fades to black instead of the fog colour
    uint tex;
    uint samp;
    float texGamma; // != 1: PC texel curve floor(256·(c/256)^g) on the 0..255 texel (Settings::texGamma)
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCol;
layout(location = 2) in float vDist;
layout(location = 0) out vec4 oCol;
void main() {
    vec4 t = texture(sampler2D(uTex[pc.tex], uSamp[pc.samp]), vUv);
    if (pc.texGamma != 1.0) t.rgb = floor(256.0 * pow(t.rgb * (255.0 / 256.0), vec3(pc.texGamma)) + 1e-3) / 255.0;
    vec4 c = t * vCol;
    c.rgb = min(c.rgb * pc.fogColorScale.a, vec3(1.0));
    if (c.a < pc.alphaRef) discard;
    if (pc.fog.y > pc.fog.x) {
        float f = clamp((pc.fog.y - vDist) / (pc.fog.y - pc.fog.x), 0.0, 1.0);
        c.rgb = mix((pc.flags & 1u) != 0u ? vec3(0.0) : pc.fogColorScale.rgb, c.rgb, f);
    }
    oCol = c;
}
