#undef NDEBUG
#include "bb/render/camera.h"

#include <cassert>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

using namespace bb::render;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

int main() {
    // Pillarbox 4:3 inside 3440x1440 ultrawide, letterbox inside a portrait window.
    Viewport p = fitAspect(3440, 1440, 4.0f / 3.0f);
    assert(p.w == 1920 && p.h == 1440 && p.x == 760 && p.y == 0);
    Viewport l = fitAspect(800, 1000, 4.0f / 3.0f);
    assert(l.w == 800 && l.h == 600 && l.x == 0 && l.y == 200);
    Viewport z = fitAspect(0, 720, 4.0f / 3.0f);
    assert(z.w == 0);

    const float v = glm::radians(60.0f);
    const float t = std::tan(v * 0.5f);
    // Hor+: vertical FOV fixed for 4:3, 16:9, 21:9 and 32:9.
    for (float a : {4.0f / 3.0f, 16.0f / 9.0f, 3440.0f / 1440.0f, 32.0f / 9.0f}) {
        assert(near(horPlusVFov(v, a), v));
        glm::mat4 m = projection(v, a, 0.1f, 100.0f);
        assert(near(m[1][1], 1.0f / t));       // vertical extent unchanged
        assert(near(m[0][0], 1.0f / (t * a))); // horizontal extent grows with aspect
    }
    // Narrower than 4:3 (portrait/5:4): horizontal FOV stays at its 4:3 value, so nothing is cropped.
    for (float a : {1.0f, 1.25f, 0.5f}) {
        glm::mat4 m = projection(v, a, 0.1f, 100.0f);
        assert(near(m[0][0], 1.0f / (t * 4.0f / 3.0f)));
        assert(horPlusVFov(v, a) > v);
    }

    // Camera: yaw 0 looks down -Z; a point straight ahead projects to the view axis.
    Camera c;
    c.pos = {1, 2, 3};
    assert(near(c.forward().z, -1.0f) && near(c.right().x, 1.0f));
    glm::vec4 ahead = c.view() * glm::vec4(c.pos + c.forward() * 5.0f, 1.0f);
    assert(near(ahead.x, 0.0f) && near(ahead.y, 0.0f) && near(ahead.z, -5.0f));
    c.yaw = glm::radians(90.0f); // turned left: looking down -X
    assert(near(c.forward().x, -1.0f) && near(c.right().z, -1.0f));
    return 0;
}
