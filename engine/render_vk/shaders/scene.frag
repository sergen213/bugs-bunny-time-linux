#version 460
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RT
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#endif
// Mirrors kFs3D of the GL renderer. Variants (CMakeLists.txt): plain = Original Fidelity / Enhanced without lighting
// (no change to the GL-matching maths); LIT = Enhanced sun shadows from the cascaded shadow map; LIT+RT = ray-query
// shadows, ray-traced ambient occlusion and reflections (docs/ROADMAP.md "Enhanced sun shadows and ray tracing");
// MV (any of them, FSR) = second output with the motion vector.
layout(set = 0, binding = 0) uniform texture2D uTex[];
layout(set = 0, binding = 1) uniform sampler uSamp[5];
layout(push_constant) uniform P {
    mat4 viewProj;
    vec4 viewZ;
    vec4 fogColorScale;
    vec2 fog;       // linear fog start, end; end <= start = off
    float alphaRef; // texels below are discarded (AlphaTest); negative = off
    uint flags;     // bit 0: additive/subtractive surface, fades to black instead of the fog colour; bit 1: PC ABR-2
                    // (bugs.exe 0x420d10 case 2: alpha = trunc luma of the raw texel, alpha-blended);
                    // LIT / MV only: bit 2 receives sun shadows, bit 3 receives ambient occlusion, bit 4 alpha-blended,
                    // bit 5 reactive (sprites, animated textures: their texels change without motion);
                    // bit 6: PC ABR 0/1/3 (bugs.exe 0x420d10 modes 0/1/3, see kFs3D), alpha-blended
    uint tex;
    uint samp;
    float texGamma; // != 1: PC texel curve floor(256·(c/256)^g) on the 0..255 texel (Settings::texGamma)
#if defined(LIT) || defined(MV)
    uint material;  // packHalf2x16(reflection strength, roughness), 0 = not reflective
#endif
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCol;
layout(location = 2) in float vDist;
layout(location = 0) out vec4 oCol;
#ifdef MV
layout(location = 5) in vec4 vCur;
layout(location = 6) in vec4 vPrev;
layout(location = 1) out vec2 oMotion; // previous minus current position, UV units (y down)
// FSR reactive / transparency-and-composition mask (blended with MAX): how little the upscaler may trust its history
// here. Additive glows, translucent surfaces (water, veils), sprites / animated textures and reflections change
// without matching motion vectors and would otherwise trail.
layout(location = 2) out float oReactive;
#endif

#ifdef LIT
layout(location = 3) in vec3 vWorld;
layout(location = 4) in vec3 vNormal;  // smooth vertex normal (unnormalised, any orientation)
// renderer.cpp LightUbo
layout(set = 1, binding = 0) uniform Light {
    mat4 cascade[4]; // world -> shadow-map clip (Vulkan z 0..1) per cascade
    vec4 splits;     // view distance where each cascade ends
    vec4 texel;      // world size of one shadow-map texel per cascade
    vec4 sun;        // xyz unit vector toward the sun, w = brightness of shadowed surfaces
    vec4 eye;        // camera position
    vec4 rt;         // x ray origin offset per unit of view distance, y shadow / reflection ray length, z AO radius, w AO strength
    uint mode;       // 0 no shadows, 1 shadow map, 2 ray-query shadows, 3 ray-query shadows + AO
    float rayMin;    // ray-query tMin (skips stacked decal polygons)
    uint reflect;    // 1: ray-traced reflections on (RT variant)
    uint temporal;   // 1: FSR accumulates frames: per-pixel random ray sets (AO, rough reflections); 0: fixed sets
    mat4 curViewProj, prevViewProj;
} L;
layout(set = 1, binding = 1) uniform sampler2DArrayShadow uShadow;

float visibleMap(vec3 p, vec3 ng) {
    int c = vDist < L.splits.x ? 0 : vDist < L.splits.y ? 1 : vDist < L.splits.z ? 2 : 3;
    if (vDist >= L.splits.w) return 1.0;
    vec4 s = L.cascade[c] * vec4(p + ng * (L.texel[c] * 1.5), 1.0);
    vec2 uv = s.xy * 0.5 + 0.5;
    vec2 t = vec2(0.5) / vec2(textureSize(uShadow, 0).xy);
    float v = 0.0; // 4 bilinear PCF taps = a 3x3 texel footprint
    for (int i = 0; i < 4; ++i) v += texture(uShadow, vec4(uv + t * vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0), float(c), s.z));
    return v * 0.25;
}

#ifdef RT
layout(set = 1, binding = 2) uniform accelerationStructureEXT uTlas;
struct Geo { uvec2 vtx, idx; uint first; int base; uint tex; uint pad; }; // renderer.cpp RtGeo
layout(set = 1, binding = 3, std430) readonly buffer Geos { Geo g[]; } G;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Floats { float f[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Uints { uint u[]; };

// Texture coordinate and vertex colour of a hit (Vertex = 6 floats: pos xyz, uv, rgba8).
void hitAttribs(Geo g, uint prim, vec2 b, out vec2 uv, out vec4 col) {
    uint first = g.first + prim * 3u;
    vec3 w = vec3(1.0 - b.x - b.y, b.x, b.y);
    Uints ib = Uints(g.idx);
    Floats vb = Floats(g.vtx);
    uv = vec2(0.0);
    col = vec4(0.0);
    for (uint k = 0u; k < 3u; ++k) {
        uint v = uint(int(ib.u[first + k]) + g.base) * 6u;
        uv += w[k] * vec2(vb.f[v + 3u], vb.f[v + 4u]);
        col += w[k] * unpackUnorm4x8(floatBitsToUint(vb.f[v + 5u]));
    }
}

// Distance to a hit on o + d·[rayMin, tmax], tmax when none (any hit with gl_RayFlagsTerminateOnFirstHitEXT, else the
// closest). Alpha-tested geometry (non-opaque in the BLAS) is resolved with its texture. hit*: the committed hit.
float trace(vec3 o, vec3 d, float tmax, uint flags, out Geo hitGeo, out uint hitPrim, out vec2 hitBary) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, uTlas, flags, 0xFF, o, L.rayMin, d, tmax);
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        Geo g = G.g[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false) + rayQueryGetIntersectionGeometryIndexEXT(q, false)];
        vec2 uv;
        vec4 col;
        hitAttribs(g, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), rayQueryGetIntersectionBarycentricsEXT(q, false), uv, col);
        if (textureLod(sampler2D(uTex[nonuniformEXT(g.tex)], uSamp[1]), uv, 0.0).a >= 0.5) rayQueryConfirmIntersectionEXT(q);
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return tmax;
    hitGeo = G.g[rayQueryGetIntersectionInstanceCustomIndexEXT(q, true) + rayQueryGetIntersectionGeometryIndexEXT(q, true)];
    hitPrim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    hitBary = rayQueryGetIntersectionBarycentricsEXT(q, true);
    return rayQueryGetIntersectionTEXT(q, true);
}
float trace(vec3 o, vec3 d, float tmax, uint flags) {
    Geo g;
    uint p;
    vec2 b;
    return trace(o, d, tmax, flags, g, p, b);
}
#endif

// Interleaved gradient noise: per-pixel rotation of the AO / reflection ray sets.
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// n: shading normal (smooth, toward the viewer); ng: geometric face normal toward the viewer. N·L, the AO hemisphere
// and reflections use n; ray origins are pushed along ng (self-intersection), and faces turned from the sun
// geometrically count as shadowed.
float sunLight(vec3 n, vec3 ng) {
    float ndl = dot(n, L.sun.xyz);
    float vis = 0.0;
    if (L.mode != 0u && ndl > 0.0 && dot(ng, L.sun.xyz) > -0.02) {
#ifdef RT
        if (L.mode >= 2u) {
            vec3 o = vWorld + ng * (L.rt.x * vDist + 0.5);
            vis = trace(o, L.sun.xyz, L.rt.y, gl_RayFlagsTerminateOnFirstHitEXT) < L.rt.y ? 0.0 : 1.0;
        } else
#endif
        vis = visibleMap(vWorld, ng);
        vis *= smoothstep(0.0, 0.12, ndl); // soft terminator instead of a hard light/shadow edge at grazing angles
    } else if (L.mode == 0u) {
        vis = 1.0;
    }
    float light = mix(L.sun.w, 1.0, vis);
#ifdef RT
    if (L.mode == 3u && (pc.flags & 8u) != 0u) { // cosine-weighted hemisphere around n, 6 short rays, closest hit fading out
        vec3 t = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 bt = cross(n, t);
        vec3 o = vWorld + ng * (L.rt.x * vDist + 0.5);
        float r = L.temporal != 0u ? ign(gl_FragCoord.xy) : 0.5, occ = 0.0;
        const int N = 6;
        for (int i = 0; i < N; ++i) {
            float u = (float(i) + r) / float(N), phi = 6.2831853 * fract(float(i) * 0.618034 + r * 0.5);
            vec3 d = (t * cos(phi) + bt * sin(phi)) * sqrt(u) + n * sqrt(1.0 - u);
            if (dot(d, ng) <= 0.0) d = reflect(d, ng); // keep the rays out of the face itself
            float f = 1.0 - trace(o, d, L.rt.z, gl_RayFlagsNoneEXT) / L.rt.z;
            occ += f * f;
        }
        light *= 1.0 - L.rt.w * occ / float(N);
    }
#endif
    return light;
}

#ifdef RT
// One-bounce ray-traced reflection over c (Schlick Fresnel, strength and roughness from pc.material). The hit is shaded
// with its texture, vertex colour and sun visibility; a miss (sky, beyond the ray length) leaves c. The result keeps the
// surface's blending: opaque mixes, alpha-blended surfaces become more opaque where they reflect (the reflection covers
// what lies behind), additive ones (LiT water layers) add it.
vec4 reflection(vec4 c, vec3 n, vec3 ng) {
    vec2 m = unpackHalf2x16(pc.material);
    vec3 v = normalize(vWorld - L.eye.xyz);
    vec3 nr = n;
    if (m.y > 0.0 && L.temporal != 0u) { // roughness, only with FSR on: its accumulation averages the per-pixel random tilts
        float a = 6.2831853 * ign(gl_FragCoord.xy), b = ign(gl_FragCoord.yx + 17.0);
        vec3 t = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        nr = normalize(n + m.y * sqrt(b) * (t * cos(a) + cross(n, t) * sin(a)));
    }
    vec3 d = reflect(v, nr);
    if (dot(d, ng) <= 0.0) d = reflect(v, ng);
    vec3 o = vWorld + ng * (L.rt.x * vDist + 0.5);
    Geo g;
    uint prim;
    vec2 bary;
    float t = trace(o, d, L.rt.y, gl_RayFlagsNoneEXT, g, prim, bary);
    if (t >= L.rt.y) return c;

    vec2 uv;
    vec4 col;
    hitAttribs(g, prim, bary, uv, col);
    vec3 hit = textureLod(sampler2D(uTex[nonuniformEXT(g.tex)], uSamp[1]), uv, 0.0).rgb * col.rgb * pc.fogColorScale.a;
    vec3 p = o + d * t;
    float lit = L.mode >= 2u ? (trace(p + L.sun.xyz * (L.rt.x * (vDist + t) + 1.0), L.sun.xyz, L.rt.y, gl_RayFlagsTerminateOnFirstHitEXT) < L.rt.y ? 0.0 : 1.0) : 1.0;
    hit = min(hit, vec3(1.0)) * mix(L.sun.w, 1.0, lit);
    float f = m.x * (0.3 + 0.7 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0));
    if ((pc.flags & 1u) != 0u) return vec4(c.rgb * c.a + hit * f, 1.0);       // additive (SRC_ALPHA, ONE)
    if ((pc.flags & 16u) != 0u) {                                             // alpha-blended: premultiplied mix
        float a = 1.0 - (1.0 - c.a) * (1.0 - f);
        return vec4((c.rgb * c.a * (1.0 - f) + hit * f) / max(a, 1e-4), a);
    }
    return vec4(mix(c.rgb, hit, f), c.a);
}
#endif
#endif

void main() {
    vec4 t = texture(sampler2D(uTex[pc.tex], uSamp[pc.samp]), vUv);
    float luma = floor(dot(t.rgb * 255.0, vec3(0.299, 0.587, 0.114)) + 1e-3) / 255.0;
    vec4 vc = vCol;
    if ((pc.flags & 64u) != 0u) {
        float mx = floor(max(t.r, max(t.g, t.b)) * 255.0 + 0.5), va = floor(vCol.a * 255.0 + 0.5);
        t.rgb += (255.0 - mx) / 255.0;
        t.a *= (va == 128.0 ? floor(mx / 2.0) : va == 64.0 ? floor(mx / 4.0) : mx - floor(mx / 4.0)) / 255.0;
        vc.a = 1.0;
    }
    if (pc.texGamma != 1.0) t.rgb = floor(256.0 * pow(t.rgb * (255.0 / 256.0), vec3(pc.texGamma)) + 1e-3) / 255.0;
    vec4 c = t * vc;
    if ((pc.flags & 2u) != 0u) c = vec4(0.0, 0.0, 0.0, luma * vCol.a);
    c.rgb = min(c.rgb * pc.fogColorScale.a, vec3(1.0));
    if (c.a < pc.alphaRef) discard;
#ifdef LIT
    if ((pc.flags & 4u) != 0u || pc.material != 0u) {
        vec3 ng = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
        if (dot(ng, L.eye.xyz - vWorld) < 0.0) ng = -ng; // face normal toward the viewer (two-sided cards included)
        vec3 n = vNormal;
        float len = length(n);
        n = len > 1e-3 ? n / len : ng;
        if (dot(n, ng) < 0.0) n = -n;                    // the vertex normal of the side being seen
        if ((pc.flags & 4u) != 0u) c.rgb *= sunLight(n, ng);
#ifdef RT
        if (L.reflect != 0u && pc.material != 0u) c = reflection(c, n, ng);
#endif
    }
#endif
    if (pc.fog.y > pc.fog.x) {
        float f = clamp((pc.fog.y - vDist) / (pc.fog.y - pc.fog.x), 0.0, 1.0);
        if ((pc.flags & 64u) != 0u) c.a *= f;
        else c.rgb = mix((pc.flags & 1u) != 0u ? vec3(0.0) : pc.fogColorScale.rgb, c.rgb, f);
    }
    oCol = c;
#ifdef MV
    oMotion = vec2(0.5, -0.5) * (vPrev.xy / vPrev.w - vCur.xy / vCur.w);
    float re = 0.0;
    if ((pc.flags & 1u) != 0u) re = max(c.r, max(c.g, c.b)) * c.a * 1.5; // additive / subtractive glow, by its strength
    else if ((pc.flags & 16u) != 0u) re = c.a;                            // alpha-blended: by its coverage
    if ((pc.flags & 32u) != 0u) re = max(re, c.a);
    if (pc.material != 0u) re = max(re, 0.5 * unpackHalf2x16(pc.material).x);
    oReactive = min(re, 0.9);                                             // FSR's recommended ceiling
#endif
}
