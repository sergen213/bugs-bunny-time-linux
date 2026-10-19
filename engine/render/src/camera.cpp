#include "bb/render/camera.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace bb::render {

Viewport fitAspect(int w, int h, float aspect) {
    if (w <= 0 || h <= 0 || !(aspect > 0.0f)) return {0, 0, std::max(w, 0), std::max(h, 0)};
    int vw = w, vh = h;
    if (float(w) / float(h) > aspect) vw = std::max(1, int(std::lround(float(h) * aspect)));
    else                              vh = std::max(1, int(std::lround(float(w) / aspect)));
    return {(w - vw) / 2, (h - vh) / 2, vw, vh};
}

float horPlusVFov(float vfov43Rad, float aspect) {
    constexpr float kBase = 4.0f / 3.0f;
    if (!(aspect > 0.0f) || aspect >= kBase) return vfov43Rad;
    return 2.0f * std::atan(std::tan(vfov43Rad * 0.5f) * kBase / aspect);
}

glm::mat4 projection(float vfov43Rad, float aspect, float zNear, float zFar) {
    return glm::perspective(horPlusVFov(vfov43Rad, aspect), aspect, zNear, zFar);
}

glm::vec3 Camera::forward() const {
    return {std::cos(pitch) * -std::sin(yaw), std::sin(pitch), std::cos(pitch) * -std::cos(yaw)};
}

glm::vec3 Camera::right() const { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }

glm::mat4 Camera::view() const { return glm::lookAt(pos, pos + forward(), glm::vec3(0, 1, 0)); }

} // namespace bb::render
