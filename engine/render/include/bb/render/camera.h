// Projection / camera math. Pure functions, no GL: unit-tested in engine/render/tests.
#pragma once
#include <glm/glm.hpp>

namespace bb::render {

struct Viewport { int x = 0, y = 0, w = 0, h = 0; };

// Largest centered rect with the given aspect (w/h) inside a w x h area (pillarbox or letterbox).
Viewport fitAspect(int w, int h, float aspect);

// Hor+: the vertical FOV (defined at 4:3) is kept for every aspect >= 4:3, so wider screens see more
// horizontally. Narrower-than-4:3 aspects keep the 4:3 horizontal FOV instead, so nothing is cropped.
float horPlusVFov(float vfov43Rad, float aspect);

glm::mat4 projection(float vfov43Rad, float aspect, float zNear, float zFar);

// Free-fly camera. Y up, yaw 0 looks down -Z, positive pitch looks up. Radians.
struct Camera {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f, pitch = 0.0f;

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::mat4 view() const;
};

} // namespace bb::render
