// LiT Original Fidelity: bugs.exe's GL blended-pass order (flush 0x4124b0, RUNTIME_REFERENCE §29). Shared by the GL and
// Vulkan renderers.
#pragma once
#include "bb/scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bb::render {

// World-space sort point of each triangle of a mesh, for translucent meshes only: the vertex mean of its polygon.
// A PS1 quad is decoded as the triangle pair (a, a+2, a+1) (a+1, a+2, a+3) (level.cpp, fresh vertices per polygon,
// so the pattern only arises from a quad); both get the 4-vertex mean the exe sorts the quad by.
inline std::vector<glm::vec3> triCentroids(const Mesh& m) {
    std::vector<glm::vec3> c;
    if (m.blend != Blend::Alpha && m.blend != Blend::Additive && m.blend != Blend::Subtract) return c;
    c.reserve(m.indices.size() / 3);
    const std::vector<uint32_t>& x = m.indices;
    auto p = [&](uint32_t i) { return glm::vec3(m.vertices[i].pos[0], m.vertices[i].pos[1], m.vertices[i].pos[2]); };
    for (size_t k = 0; k + 2 < x.size(); k += 3) {
        const uint32_t a = x[k];
        if (k + 5 < x.size() && x[k + 1] == a + 2 && x[k + 2] == a + 1 && x[k + 3] == a + 1 && x[k + 4] == a + 2 && x[k + 5] == a + 3) {
            c.insert(c.end(), 2, (p(a) + p(a + 1) + p(a + 2) + p(a + 3)) / 4.0f);
            k += 3;
        } else {
            c.push_back((p(a) + p(x[k + 1]) + p(x[k + 2])) / 3.0f);
        }
    }
    return c;
}

struct PcTri { float z; uint32_t mesh, tri; };  // z = eye-space z of the centroid (negative in front of the eye)

// bugs.exe sorts every polygon of the blended layer-1 lists by the mean eye-space z of its vertices, ascending =
// farthest first (0x413440, glOrtho near 400 / far 65536 so eye z < 0), and draws them in that order with depth
// writes on (glDepthMask is never called) and the alpha test off (0x4ac030 = 0). Calls draw(mesh, firstTri, count)
// for each run of consecutive triangles of one mesh.
// Both triangles of a quad share its sort key, so the stable sort keeps them adjacent in one draw run.
template <class Draw> void drawPcSorted(std::vector<PcTri>& tris, Draw&& draw) {
    std::ranges::stable_sort(tris, {}, &PcTri::z);
    for (size_t i = 0; i < tris.size();) {
        size_t j = i + 1;
        while (j < tris.size() && tris[j].mesh == tris[i].mesh && tris[j].tri == tris[j - 1].tri + 1) ++j;
        draw(tris[i].mesh, tris[i].tri, uint32_t(j - i));
        i = j;
    }
}

}  // namespace bb::render
