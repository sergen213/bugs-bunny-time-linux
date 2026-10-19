#undef NDEBUG
#include "fixed_step.h"

#include <cassert>

int main() {
    bb::FixedStep fs{.dt = 0.1};
    assert(fs.advance(0.05) == 0 && fs.alpha() > 0.49f && fs.alpha() < 0.51f);
    assert(fs.advance(0.06) == 1 && fs.alpha() < 0.11f);
    int total = 0;
    for (int i = 0; i < 100; ++i) total += fs.advance(0.01);
    assert(total == 10);                             // 1 s of 10 ms frames at 10 Hz
    assert(fs.advance(60.0) == fs.maxTicks);         // stall: capped
    assert(fs.alpha() >= 0.0f && fs.alpha() < 1.0f); // backlog dropped
    assert(fs.advance(-1.0) == 0);
    return 0;
}
