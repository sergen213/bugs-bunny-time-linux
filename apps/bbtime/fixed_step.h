// Fixed-timestep accumulator: simulation runs at a constant tick rate, rendering interpolates
// between the previous and current tick with alpha() in [0, 1).
#pragma once
#include <cmath>

namespace bb {

// 30 Hz fixed tick: observed statically in both exes (frame pacer, docs/re/GAMEPLAY.md §1.1).
inline constexpr double kTickHz = 30.0;

struct FixedStep {
    double dt = 1.0 / kTickHz;
    double acc = 0.0;
    int maxTicks = 8; // after a long stall, drop the backlog instead of spiralling

    // Adds real elapsed time, returns how many ticks to simulate now.
    int advance(double seconds) {
        acc += seconds > 0.0 ? seconds : 0.0;
        int n = 0;
        while (acc >= dt && n < maxTicks) {
            acc -= dt;
            ++n;
        }
        if (acc >= dt) acc = std::fmod(acc, dt);
        return n;
    }
    float alpha() const { return float(acc / dt); }
};

} // namespace bb
