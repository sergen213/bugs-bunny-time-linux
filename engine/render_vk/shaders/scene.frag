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
                    // bit 6: PC ABR 0/1/3 (bugs.exe 0x420d10 modes 0/1/3, see kFs3D), alpha-blended;
                    // LIT only: bit 7 Enhanced stylised water (shadeWater)
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
    mat4 invViewProj; // render-target uv + depth -> world (water)
    vec4 waves;       // wave field: xy origin (world xz), z 1/(cell·N), w cell (0 = none)
    vec4 water;       // x animation seconds, yz 1 / render-target size
    uvec4 waterSlots; // bindless uTex: opaque colour copy, its depth, wave heights, normal map
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
// Colour of the first hit along o + d·[rayMin, rt.y] (texture x vertex colour x scene colour scale, sun visibility); false
// on a miss (sky, beyond the ray length). dist: view distance of o (ray offsets).
bool traceColor(vec3 o, vec3 d, float dist, out vec3 hit) {
    Geo g;
    uint prim;
    vec2 bary;
    float t = trace(o, d, L.rt.y, gl_RayFlagsNoneEXT, g, prim, bary);
    if (t >= L.rt.y) return false;
    vec2 uv;
    vec4 col;
    hitAttribs(g, prim, bary, uv, col);
    hit = textureLod(sampler2D(uTex[nonuniformEXT(g.tex)], uSamp[1]), uv, 0.0).rgb * col.rgb * pc.fogColorScale.a;
    vec3 p = o + d * t;
    float lit = L.mode >= 2u ? (trace(p + L.sun.xyz * (L.rt.x * (dist + t) + 1.0), L.sun.xyz, L.rt.y, gl_RayFlagsTerminateOnFirstHitEXT) < L.rt.y ? 0.0 : 1.0) : 1.0;
    hit = min(hit, vec3(1.0)) * mix(L.sun.w, 1.0, lit);
    return true;
}

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
    vec3 hit;
    if (!traceColor(vWorld + ng * (L.rt.x * vDist + 0.5), d, vDist, hit)) return c;
    float f = m.x * (0.3 + 0.7 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0));
    if ((pc.flags & 1u) != 0u) return vec4(c.rgb * c.a + hit * f, 1.0);       // additive (SRC_ALPHA, ONE)
    if ((pc.flags & 16u) != 0u) {                                             // alpha-blended: premultiplied mix
        float a = 1.0 - (1.0 - c.a) * (1.0 - f);
        return vec4((c.rgb * c.a * (1.0 - f) + hit * f) / max(a, 1e-4), a);
    }
    return vec4(mix(c.rgb, hit, f), c.a);
}
#endif

// ---- Enhanced stylised water (Settings::water, docs/ROADMAP.md "Stylised interactive water") ----------------------------
// Ported from the Joan of Arc port's shade_water: saturated teal-to-deep-blue absorption over the bed seen through the
// surface (the opaque scene and its depth, copied before the water is drawn, refracted by the waves), a soft shoreline
// foam band, two scrolling layers of a procedural normal map, the interactive wave field (renderer.cpp waterWaves),
// Fresnel reflection (ray-traced, else screen-space over the copy, else the sky seen at the top of the screen) and sun
// glints. Joan's distances scale by kWU (Bugs ~300 units tall, Joan's units ~80). Shaded in linear light over the
// sRGB-encoded scene; drawn opaque, so the waterline has no hard edge.
const mat2 kWaterRot = mat2(0.8, 0.6, -0.6, 0.8);
const float kWU = 3.75;
vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toSrgb(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 copyColor(vec2 uv) { return toLinear(textureLod(sampler2D(uTex[L.waterSlots.x], uSamp[3]), uv, 0.0).rgb); }
float copyDepth(vec2 uv) { return textureLod(sampler2D(uTex[L.waterSlots.y], uSamp[2]), uv, 0.0).r; }
vec3 worldAt(vec2 uv, float depth) { // uv: render-target (top-left origin); the viewport's negative height flips y
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    if (depth >= 1.0) { // nothing drawn (a void under the water, CC2A river): far along the view ray, not 1/0 at far
        vec4 a = L.invViewProj * vec4(ndc, 0.0, 1.0), b = L.invViewProj * vec4(ndc, 0.5, 1.0);
        return a.xyz / a.w + normalize(b.xyz / b.w - a.xyz / a.w) * 1.0e5;
    }
    vec4 p = L.invViewProj * vec4(ndc, depth, 1.0);
    return p.xyz / p.w;
}

// Interactive waves: xy the height gradient (world xz, central differences 1.5 cells apart, so the cell-sized
// checkerboard bodies stamp cancels), z the height, w its Laplacian (negative on crests); 0 outside the field (faded
// over its outer 4 %) or without one.
vec4 waterWaves(vec2 p) {
    if (L.waves.w == 0.0) return vec4(0.0);
    vec2 uv = (p - L.waves.xy) * L.waves.z;
    vec2 inside = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(inside.x, inside.y));
    if (fade == 0.0) return vec4(0.0);
    float d = 1.5 * L.waves.w, du = d * L.waves.z;
    #define WAVE(o) textureLod(sampler2D(uTex[L.waterSlots.z], uSamp[3]), uv + (o), 0.0).r
    float h = WAVE(vec2(0.0)), x0 = WAVE(vec2(-du, 0.0)), x1 = WAVE(vec2(du, 0.0)), z0 = WAVE(vec2(0.0, -du)), z1 = WAVE(vec2(0.0, du));
    #undef WAVE
    return fade * vec4(vec2(x1 - x0, z1 - z0) / (2.0 * d), h, (x0 + x1 + z0 + z1 - 4.0 * h) / (d * d));
}

// Smooth value noise in [0, 1], one lattice cell per unit of x (repeats every 289 cells: keeps sin's argument small).
float latticeHash(vec2 c) { return fract(sin(dot(mod(c, 289.0), vec2(12.9898, 78.233))) * 43758.5453); }
float valueNoise(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(latticeHash(i), latticeHash(i + vec2(1.0, 0.0)), f.x), mix(latticeHash(i + vec2(0.0, 1.0)), latticeHash(i + 1.0), f.x), f.y);
}
float dGgx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}
float vSmith(float nv, float nl, float a) {
    float k = a * 0.5;
    return 0.25 / max((nv * (1.0 - k) + k) * (nl * (1.0 - k) + k), 1e-4);
}

// Sun visibility of a water point (glints): the frame's shadow mode, lit without one.
float waterSun(vec3 p) {
    const vec3 up = vec3(0.0, 1.0, 0.0);
#ifdef RT
    if (L.mode >= 2u) return trace(p + up * (L.rt.x * vDist + 0.5), L.sun.xyz, L.rt.y, gl_RayFlagsTerminateOnFirstHitEXT) < L.rt.y ? 0.0 : 1.0;
#endif
    return L.mode == 1u ? visibleMap(p, up) : 1.0;
}

// Screen-space mirror ray over the opaque copy: marches r from p until it passes behind the copied depth; a hit within
// the step's thickness returns its colour (a = 1, faded at the screen edges), else a = 0 (off screen, occluded).
vec4 waterSsr(vec3 p, vec3 r, float dist) {
    float t = 0.03 * dist + 20.0;
    for (int i = 0; i < 28; ++i, t *= 1.22) {
        vec4 c = pc.viewProj * vec4(p + r * t, 1.0);
        if (c.w <= 0.0) break;
        vec3 ndc = c.xyz / c.w;
        vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float d = copyDepth(uv);
        if (ndc.z <= d) continue;
        if (distance(p + r * t, L.eye.xyz) - distance(worldAt(uv, d), L.eye.xyz) > 0.3 * t + 2.0 * kWU) break; // behind a thin object
        vec2 e = min(uv, 1.0 - uv);
        return vec4(copyColor(uv), smoothstep(0.0, 0.06, min(e.x, e.y)));
    }
    return vec4(0.0);
}

// p: world position of the surface; light: the brightness the level's baked lighting gives the water. Returns sRGB.
vec3 shadeWater(vec3 p, float light) {
    float time = L.water.x;
    vec2 wa = p.xz * (1.0 / (900.0 * kWU)) + time * vec2(0.016, 0.009);
    vec2 wb = kWaterRot * p.xz * (1.0 / (260.0 * kWU)) + time * vec2(-0.030, 0.041);
    vec4 na = texture(sampler2D(uTex[L.waterSlots.w], uSamp[1]), wa), nb = texture(sampler2D(uTex[L.waterSlots.w], uSamp[1]), wb);
    vec3 ta = na.xyz * 2.0 - 1.0, tb = nb.xyz * 2.0 - 1.0;
    vec2 s = 0.4 * ta.xy / max(ta.z, 0.3) + 0.35 * (transpose(kWaterRot) * tb.xy) / max(tb.z, 0.3);
    // Wave slopes tilt the normal about 1:1, softly capped towards 0.3 (steeper tilts swing the reflection across bank /
    // sky edges into hard-edged blotches). A height field's normal is (-dh/dx, 1, -dh/dz).
    vec4 wave = waterWaves(p.xz);
    wave.xy *= 1.2 / (1.0 + 4.0 * length(wave.xy));
    s -= wave.xy;
    // Calmer with distance: the ripples shrink below a pixel near the horizon, where their reflection only sparkles.
    float dist = distance(L.eye.xyz, p);
    s /= 1.0 + dist / (2000.0 * kWU);
    vec3 n = normalize(vec3(s.x, 1.0, s.y));
    vec3 v = normalize(L.eye.xyz - p);
    float nv = max(dot(n, v), 1e-3);

    // Bed under the surface: vertical depth (shoreline) and, along the refracted view, the path length through the water.
    vec2 uv = gl_FragCoord.xy * L.water.yz;
    float depth = max(p.y - worldAt(uv, copyDepth(uv)).y, 0.0);
    float far = 1.0 + dist / (1500.0 * kWU);
    vec2 ruv = uv + s * (0.025 * smoothstep(0.0, 80.0 * kWU, depth) / far) - wave.xy * (0.015 * smoothstep(0.0, 8.0 * kWU, depth) / far);
    vec3 bed = worldAt(ruv, copyDepth(ruv));
    if (bed.y > p.y) { // the offset sample is above the water (shore, a foreground object): look straight through
        ruv = uv;
        bed = worldAt(uv, copyDepth(uv));
    }
    vec3 below = copyColor(ruv);
    float path = distance(p, bed), bedDepth = max(p.y - bed.y, 0.0);

    // Absorption (red first: teal shallows) + in-scattered colour from teal to deep blue, lit like the level.
    vec3 lightC = vec3(light);
    vec3 tr = exp(-vec3(0.0080, 0.0026, 0.0019) / kWU * path);
    vec3 scatter = mix(vec3(0.05, 0.32, 0.30), vec3(0.006, 0.05, 0.11), smoothstep(20.0 * kWU, 260.0 * kWU, bedDepth)) * lightC;
    vec3 c = below * tr + scatter * (1.0 - tr);

    // Reflection with Schlick Fresnel; sun glints on the ripples. Both fade in over the first units of depth so the
    // waterline stays soft. Sky: the top of the screen (usually the level's sky dome), half a stylised sky gradient.
    float edge = smoothstep(0.0, 10.0 * kWU, depth);
    vec3 r = reflect(-v, n);
    r.y = abs(r.y);
    vec3 top = vec3(0.0);
    for (int i = 0; i < 5; ++i) top += copyColor(vec2(0.1 + 0.2 * float(i), 0.03)) * 0.2;
    vec3 sky = mix(top, mix(vec3(0.45, 0.6, 0.75), vec3(0.15, 0.32, 0.6), r.y) * lightC, 0.5);
    vec3 refl = sky;
    vec4 ssr = waterSsr(p, r, dist);
    refl = mix(refl, ssr.rgb, ssr.a);
#ifdef RT
    vec3 hit;
    if (L.reflect != 0u && traceColor(p + vec3(0.0, L.rt.x * vDist + 0.5, 0.0), normalize(vec3(r.x, max(r.y, 0.02), r.z)), vDist, hit))
        refl = toLinear(hit);
#endif
    c = mix(c, refl, (0.02 + 0.98 * pow(1.0 - nv, 5.0)) * edge);
    // Stylised wave light: crests catch the sky, troughs darken (readable from above, where Fresnel is ~0.02). Driven by
    // the curvature (a high-pass), so whole-pond sloshing and a wake's raised wedge stay a tilt and the fronts read as
    // lines. Crests go half to the sky gradient, so they do not carry the hard edges of the reflection.
    // Cartoon (L.water.w = 1, Water::Cartoon): the same water toon-shaded: crisp crest lines, hard glints, solid foam
    // with a white rim along every shore, and the colour posterised into a few luminance bands (hue kept).
    bool toon = L.water.w > 0.5;
    float bend = -100.0 * kWU * wave.w;
    float crest = sign(bend) * (toon ? smoothstep(0.9, 1.1, abs(bend)) : smoothstep(0.4, 3.0, abs(bend))) * edge;
    c = mix(c, crest > 0.0 ? (toon ? vec3(0.9) * lightC : mix(refl, sky, 0.5)) : c * 0.4, (toon ? 0.8 : 0.6) * abs(crest));
    vec3 l = L.sun.xyz, h = normalize(l + v);
    float nl = max(dot(n, l), 0.0), a = 0.07;
    float spec = dGgx(max(dot(n, h), 0.0), a) * vSmith(nv, nl, a) * nl * (0.02 + 0.98 * pow(1.0 - max(dot(v, h), 0.0), 5.0));
    if (toon) spec = 1.5 * smoothstep(1.5, 2.0, spec);
    c += spec * lightC * waterSun(p) * edge;

    // Shoreline foam: a band over the shallow bed, broken into drifting lace by fine noise. Wave foam: splash craters or
    // the sharpest crests, broken up by the normal map's height pattern.
    float band = 1.0 - smoothstep(0.0, 25.0 * kWU, depth), pattern = 0.5 * (na.a + nb.a);
    float lace = 0.6 * valueNoise(p.xz / (18.0 * kWU) + time * vec2(0.25, -0.15)) + 0.4 * valueNoise(p.xz / (7.0 * kWU) - time * vec2(0.2, 0.3));
    float foam = band * smoothstep(0.5, 0.75, lace);
    float churn = max(smoothstep(4.0 * kWU, 7.0 * kWU, abs(wave.z)), smoothstep(0.12 / kWU, 0.25 / kWU, -wave.w));
    foam = max(foam, churn * smoothstep(0.1, 0.7, pattern));
    if (toon) foam = max(smoothstep(0.3, 0.4, foam), 1.0 - smoothstep(5.0 * kWU, 7.0 * kWU, depth + 3.0 * kWU * lace));
    c = mix(c, lightC * (toon ? 0.95 : 0.85), foam * (toon ? 0.9 : 0.6) * smoothstep(0.0, 4.0 * kWU, depth));
    vec3 o = toSrgb(c);
    if (toon) { // 5 luminance bands with a narrow soft step between them (no stair aliasing)
        float y = max(dot(o, vec3(0.299, 0.587, 0.114)), 1e-3), q = y * 5.0;
        o *= (floor(q) + smoothstep(0.42, 0.58, fract(q))) / 5.0 / y;
    }
    return o;
}
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
    if ((pc.flags & 128u) != 0u) {
        // The level's baked light on the water (vertex colour x scene colour scale) sets its scattered colour, foam and glints.
        c = vec4(shadeWater(vWorld, clamp(max(vCol.r, max(vCol.g, vCol.b)) * pc.fogColorScale.a, 0.3, 1.0)), 1.0);
    } else if ((pc.flags & 4u) != 0u || pc.material != 0u) {
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
    if ((pc.flags & 128u) != 0u) re = 0.4;                               // stylised water: scrolling ripples, refraction
    if (pc.material != 0u) re = max(re, 0.5 * unpackHalf2x16(pc.material).x);
    oReactive = min(re, 0.9);                                             // FSR's recommended ceiling
#endif
}
