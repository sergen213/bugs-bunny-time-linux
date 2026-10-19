// Projection / camera math. Pure functions, no GL: unit-tested in engine/render/tests.
#pragma once
#include <cmath>
#include <glm/glm.hpp>

namespace bb::render {

struct Viewport { int x = 0, y = 0, w = 0, h = 0; };

// Largest centered rect with the given aspect (w/h) inside a w x h area (pillarbox or letterbox).
Viewport fitAspect(int w, int h, float aspect);

// Hor+: the vertical FOV (defined at 4:3) is kept for every aspect >= 4:3, so wider screens see more
// horizontally. Narrower-than-4:3 aspects keep the 4:3 horizontal FOV instead, so nothing is cropped.
float horPlusVFov(float vfov43Rad, float aspect);

glm::mat4 projection(float vfov43Rad, float aspect, float zNear, float zFar);

// Original Fidelity vertical FOV (observed-static + observed-runtime, RUNTIME_REFERENCE.md §5). Both exes project
// y_screen = H·y/z·sy/4096 with H = 400 (LiT 0x4efb30, TB 0x639a70) and sy = 4096·h/384 (LiT 0x4efb56, TB 0x639a8e;
// x likewise with sx = 4096·w/512): the view always spans the PS1 512×384 reference at H = 400, so the half-height
// h/2 subtends atan((h/2) / (400·h/384)) = atan(192/400) at every resolution -> 2·atan(0.48) = 51.28° vertical
// (2·atan(256/400) = 65.24° horizontal at 4:3). Runtime at 640×480: sy = 5120 (1.25), H = 400 -> 2·atan(240/500).
inline const float kOriginalVFovRad = 2.0f * std::atan(192.0f / 400.0f);

// Free-fly camera. Y up, yaw 0 looks down -Z, positive pitch looks up. Radians.
struct Camera {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f, pitch = 0.0f;

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::mat4 view() const;
};

} // namespace bb::render
