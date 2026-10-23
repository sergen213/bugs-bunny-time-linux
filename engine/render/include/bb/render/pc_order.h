// LiT Original Fidelity: bugs.exe's GL blended-pass order (flush 0x4124b0, RUNTIME_REFERENCE §29). Shared by the GL and
// Vulkan renderers.
#pragma once
#include "bb/scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bb::render {

// World-space centroid of each triangle of a mesh (its sort key source), for translucent meshes only.
inline std::vector<glm::vec3> triCentroids(const Mesh& m) {
    std::vector<glm::vec3> c;
    if (m.blend != Blend::Alpha && m.blend != Blend::Additive && m.blend != Blend::Subtract) return c;
    c.reserve(m.indices.size() / 3);
    auto p = [&](uint32_t i) { return glm::vec3(m.vertices[i].pos[0], m.vertices[i].pos[1], m.vertices[i].pos[2]); };
    for (size_t k = 0; k + 2 < m.indices.size(); k += 3) c.push_back((p(m.indices[k]) + p(m.indices[k + 1]) + p(m.indices[k + 2])) / 3.0f);
    return c;
}

struct PcTri { float z; uint32_t mesh, tri; };  // z = eye-space z of the centroid (negative in front of the eye)

// bugs.exe sorts every polygon of the blended layer-1 lists by the mean eye-space z of its vertices, ascending =
// farthest first (0x413440, glOrtho near 400 / far 65536 so eye z < 0), and draws them in that order with depth
// writes on (glDepthMask is never called) and the alpha test off (0x4ac030 = 0). Calls draw(mesh, firstTri, count)
// for each run of consecutive triangles of one mesh.
// ponytail: bbtime has no quads; a split quad sorts as two triangles (the exe uses the 4-vertex mean).
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
