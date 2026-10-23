#include "bb/render/water_waves.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bb::render {
namespace {

constexpr float kUnit = 300.0f / 80.0f;  // Bugs' character height over Joan's: every Joan distance scales by it
// Velocity damping (1/s): amplitude falls as exp(-1.25 t), so a splash is gone in ~2-3 s and a pond does not fill up
// with reverberating reflections.
constexpr float kDamping = 2.5f;
// Viscous damping of the velocity Laplacian: eats the cell-sized checkerboard moving bodies stamp into the field.
constexpr float kViscosity = 0.005f;
// Weak spring back to the rest level: bodies zero the cells they step into, nothing else would drain that offset.
constexpr float kRelax = 1e-4f;
// Wake: a bow mound / stern trough pair (kicks of width kWakeSigma just outside the body) deepened by kWake per unit
// moved, the speed capped at kMaxSpeed (a teleport is no sprint); slower than kMinSpeed only the body is a wall.
constexpr float kWakeSigma = 6.0f * kUnit, kWake = 0.3f, kMaxSpeed = 500.0f * kUnit, kMinSpeed = 30.0f * kUnit;
// WaterSurfaces: ground within this band of the surface makes a shore wall (slack below for ground at the water line).
constexpr float kBankBelow = 2.0f * kUnit, kBankAbove = 600.0f;
constexpr size_t kMaxSurfaceCells = size_t(1) << 20;

}  // namespace

// ---------------------------------------------------------------- WaterSurfaces

int WaterSurfaces::index(glm::vec2 xz) const {
    if (h_.empty()) return -1;
    const glm::ivec2 c(glm::floor((xz - origin_) / cell_));
    return c.x < 0 || c.y < 0 || c.x >= nx_ || c.y >= nz_ ? -1 : c.y * nx_ + c.x;
}

void WaterSurfaces::build(std::span<const glm::vec3> water, std::span<const glm::vec3> ground) {
    h_.clear();
    dry_.clear();
    if (water.size() < 3) return;
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const glm::vec3& p : water) lo = glm::min(lo, glm::vec2(p.x, p.z)), hi = glm::max(hi, glm::vec2(p.x, p.z));
    // Cells on WaveField's lattice (4 wave cells each), coarser for huge seas so the grid stays <= 1M cells.
    cell_ = 4.0f * WaveField::kCell;
    while (size_t((hi.x - lo.x) / cell_ + 2) * size_t((hi.y - lo.y) / cell_ + 2) > kMaxSurfaceCells) cell_ *= 2.0f;
    origin_ = glm::floor(lo / cell_) * cell_;
    nx_ = int((hi.x - origin_.x) / cell_) + 1;
    nz_ = int((hi.y - origin_.y) / cell_) + 1;
    h_.assign(size_t(nx_) * size_t(nz_), std::numeric_limits<float>::quiet_NaN());
    dry_.assign(h_.size(), 0);
    // Calls f(cell index, y) for every cell whose centre lies inside triangle t (xz), y = the plane there.
    auto raster = [&](const glm::vec3* t, auto&& f) {
        const glm::vec2 a(t[0].x, t[0].z), b(t[1].x, t[1].z), c(t[2].x, t[2].z);
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-3f) return;  // vertical
        const glm::vec2 tlo = glm::min(a, glm::min(b, c)), thi = glm::max(a, glm::max(b, c));
        const int x0 = std::max(0, int(std::floor((tlo.x - origin_.x) / cell_ - 0.5f))), x1 = std::min(nx_ - 1, int((thi.x - origin_.x) / cell_));
        const int z0 = std::max(0, int(std::floor((tlo.y - origin_.y) / cell_ - 0.5f))), z1 = std::min(nz_ - 1, int((thi.y - origin_.y) / cell_));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p = origin_ + (glm::vec2(x, z) + 0.5f) * cell_;
                const float w1 = ((p.x - a.x) * (c.y - a.y) - (p.y - a.y) * (c.x - a.x)) / area;
                const float w2 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / area;
                if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f) continue;
                f(size_t(z * nx_ + x), t[0].y + w1 * (t[1].y - t[0].y) + w2 * (t[2].y - t[0].y));
            }
    };
    for (size_t i = 0; i + 2 < water.size(); i += 3)
        raster(&water[i], [&](size_t k, float y) { h_[k] = std::isnan(h_[k]) ? y : std::max(h_[k], y); });
    for (size_t i = 0; i + 2 < ground.size(); i += 3) {
        const glm::vec3* t = &ground[i];
        const glm::vec2 tlo = glm::min(glm::vec2(t[0].x, t[0].z), glm::min(glm::vec2(t[1].x, t[1].z), glm::vec2(t[2].x, t[2].z)));
        const glm::vec2 thi = glm::max(glm::vec2(t[0].x, t[0].z), glm::max(glm::vec2(t[1].x, t[1].z), glm::vec2(t[2].x, t[2].z)));
        if (thi.x < origin_.x || thi.y < origin_.y || tlo.x > origin_.x + float(nx_) * cell_ || tlo.y > origin_.y + float(nz_) * cell_) continue;
        raster(t, [&](size_t k, float y) {
            if (!std::isnan(h_[k]) && y >= h_[k] - kBankBelow && y <= h_[k] + kBankAbove) dry_[k] = 1;
        });
    }
}

std::optional<float> WaterSurfaces::height(glm::vec2 xz) const {
    const int i = index(xz);
    if (i < 0 || std::isnan(h_[size_t(i)])) return std::nullopt;
    return h_[size_t(i)];
}

bool WaterSurfaces::wet(glm::vec2 xz) const {
    const int i = index(xz);
    return i >= 0 && !std::isnan(h_[size_t(i)]) && !dry_[size_t(i)];
}

// ---------------------------------------------------------------- WaveField

WaveField::WaveField(std::function<bool(glm::vec2)> wet)
    : wet_(std::move(wet)), h_(kN * kN), prev_(kN * kN), open_(kN * kN), mask_(kN * kN), scratch_(kN * kN) {
    bodies_.reserve(256);
}

int WaveField::cellIndex(glm::vec2 p) const {
    if (!placed_) return -1;
    const glm::ivec2 c = glm::ivec2(glm::floor(p / kCell)) - corner_;
    return c.x < 0 || c.y < 0 || c.x >= kN || c.y >= kN ? -1 : c.y * kN + c.x;
}

void WaveField::shift(std::vector<float>& a, int dx, int dz, float fill) {
    for (int z = 0; z < kN; ++z) {
        float* dst = scratch_.data() + z * kN;
        const int oz = z + dz;
        if (oz < 0 || oz >= kN) {
            std::fill(dst, dst + kN, fill);
            continue;
        }
        for (int x = 0; x < kN; ++x) dst[x] = x + dx >= 0 && x + dx < kN ? a[size_t(oz * kN + x + dx)] : fill;
    }
    a.swap(scratch_);
}

void WaveField::follow(glm::vec2 centre) {
    const glm::ivec2 corner = glm::ivec2(glm::floor(centre / kCell)) - kN / 2;
    if (placed_ && corner == corner_) return;
    const glm::ivec2 d = corner - corner_;
    if (!placed_ || std::abs(d.x) >= kN || std::abs(d.y) >= kN) {
        std::ranges::fill(h_, 0.0f);
        std::ranges::fill(prev_, 0.0f);
        std::ranges::fill(open_, -1.0f);
    } else {
        shift(h_, d.x, d.y, 0.0f);
        shift(prev_, d.x, d.y, 0.0f);
        shift(open_, d.x, d.y, -1.0f);
        // Rows 0 / kN-1 are never stepped (walls): clear what scrolled onto them.
        for (std::vector<float>* a : {&h_, &prev_}) {
            std::fill_n(a->begin(), kN, 0.0f);
            std::fill_n(a->end() - kN, kN, 0.0f);
        }
    }
    corner_ = corner, placed_ = true;
    for (int i = 0; i < kN * kN; ++i)
        if (open_[size_t(i)] < 0.0f)
            open_[size_t(i)] = wet_((glm::vec2(corner_ + glm::ivec2(i % kN, i / kN)) + 0.5f) * kCell) ? 1.0f : 0.0f;
}

void WaveField::kick(glm::vec2 p, float amplitude, float sigma) {
    if (!placed_) return;
    if (idle_ > kQuiet) {  // waking: the damped remainder is cleared
        std::ranges::fill(h_, 0.0f);
        std::ranges::fill(prev_, 0.0f);
    }
    idle_ = 0.0f;
    const glm::vec2 local = p / kCell - glm::vec2(corner_);  // in cells
    const float reach = 4.0f * sigma / kCell, k = kCell * kCell / (2.0f * sigma * sigma);
    const int x0 = std::max(1, int(std::floor(local.x - reach))), x1 = std::min(kN - 2, int(std::ceil(local.x + reach)));
    const int z0 = std::max(1, int(std::floor(local.y - reach))), z1 = std::min(kN - 2, int(std::ceil(local.y + reach)));
    // Free: open water, not under a body of this frame (a kick at a wader's feet would otherwise lose its crater to the
    // body and leave the rim: the level creeps up).
    const auto free = [&](int x, int z) {
        if (open_[size_t(z * kN + x)] <= 0.0f) return false;
        for (const glm::vec3& b : bodies_)
            if (glm::distance(glm::vec2(x, z) + 0.5f, glm::vec2(b) / kCell - glm::vec2(corner_)) <= b.z / kCell) return false;
        return true;
    };
    const auto shape = [&](int x, int z) {
        const glm::vec2 o = glm::vec2(x, z) + 0.5f - local;
        const float q = glm::dot(o, o) * k;
        return q > 8.0f ? 0.0f : -amplitude * (1.0f - q) * std::exp(-q);
    };
    // The Mexican hat clipped by walls and bodies keeps zero volume: what it lost comes off its free cells evenly.
    float volume = 0.0f;
    int cells = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, z); v != 0.0f && free(x, z)) volume += v, ++cells;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, z); v != 0.0f && free(x, z)) {
                // Pure displacement (h and prev alike): the crater and its rim fall back and run out as rings.
                const size_t i = size_t(z * kN + x);
                h_[i] += v - volume / float(cells), prev_[i] += v - volume / float(cells);
            }
}

void WaveField::wade(glm::vec2 p, float r, glm::vec2 move, float dt) {
    if (bodies_.size() < bodies_.capacity()) bodies_.push_back({p, r});  // ponytail: > 256 waders lose their wall
    const float dist = glm::length(move);
    if (dt <= 0.0f || dist < kMinSpeed * dt) return;
    const glm::vec2 ahead = move / dist * (r + kWakeSigma);
    const float push = kWake * std::min(dist, kMaxSpeed * dt);
    kick(p + ahead, -push, kWakeSigma);
    kick(p - ahead, push, kWakeSigma);
}

void WaveField::ripple(glm::vec2 p, float strength) {
    // Strength 1 (splash): a 26-unit crater of width 37; 0.15 (a standing body's bob) 6.7 over 25 (^0.7: faint kicks
    // still read, a crowd's bobbing does not churn the whole pond).
    kick(p, 7.0f * kUnit * std::pow(strength, 0.7f), (6.0f + 4.0f * strength) * kUnit);
}

void WaveField::step(float dt) {
    idle_ += dt;
    if (idle_ > kQuiet || !placed_) {
        bodies_.clear();
        return;
    }
    owed_ = std::min(owed_ + dt, 0.25f);  // a hitch does not run seconds of substeps
    if (owed_ >= kStep) {
        mask_ = open_;
        for (float& m : mask_) m = std::max(m, 0.0f);
        for (float* row : {mask_.data(), mask_.data() + (kN - 1) * kN}) std::fill_n(row, kN, 0.0f);
        for (int z = 0; z < kN; ++z) mask_[size_t(z * kN)] = mask_[size_t(z * kN + kN - 1)] = 0.0f;
        for (const glm::vec3& b : bodies_) {
            const glm::vec2 local = glm::vec2(b) / kCell - glm::vec2(corner_);
            const float r = b.z / kCell;
            for (int z = std::max(0, int(local.y - r)); z <= std::min(kN - 1, int(local.y + r)); ++z)
                for (int x = std::max(0, int(local.x - r)); x <= std::min(kN - 1, int(local.x + r)); ++x)
                    if (glm::distance(glm::vec2(x, z) + 0.5f, local) <= r) mask_[size_t(z * kN + x)] = 0.0f;
        }
    }
    bodies_.clear();
    // Verlet: h' = h + v * damping + (c dt / cell)^2 * lap(h) + viscosity * lap(v) - relax * h, v = h - h_prev.
    // Neighbours behind a wall count as the cell itself (zero gradient into the wall), so waves reflect there without
    // a phase flip.
    constexpr float a = (kSpeed * kStep / kCell) * (kSpeed * kStep / kCell), damp = 1.0f - kDamping * kStep;
    for (; owed_ >= kStep; owed_ -= kStep) {
        const float* h = h_.data();
        const float* p = prev_.data();
        const float* m = mask_.data();
        float* out = scratch_.data();
        std::fill_n(out, kN, 0.0f);
        std::fill_n(out + kN * (kN - 1), kN, 0.0f);
        for (int i = kN; i < kN * (kN - 1); ++i) {
            const float c = h[i], v = c - p[i];
            const float lap = m[i - 1] * (h[i - 1] - c) + m[i + 1] * (h[i + 1] - c) + m[i - kN] * (h[i - kN] - c) +
                              m[i + kN] * (h[i + kN] - c);
            const float lapv = m[i - 1] * (h[i - 1] - p[i - 1] - v) + m[i + 1] * (h[i + 1] - p[i + 1] - v) +
                               m[i - kN] * (h[i - kN] - p[i - kN] - v) + m[i + kN] * (h[i + kN] - p[i + kN] - v);
            out[i] = m[i] * (c + v * damp + a * lap + kViscosity * lapv - kRelax * c);
        }
        prev_.swap(h_);    // prev <- h
        h_.swap(scratch_);  // h <- out
    }
}

float WaveField::height(glm::vec2 p) const {
    const int i = cellIndex(p);
    return i < 0 ? 0.0f : h_[size_t(i)];
}

bool WaveField::wall(glm::vec2 p) const {
    const int i = cellIndex(p);
    return i < 0 || i < kN || i >= kN * (kN - 1) || i % kN == 0 || i % kN == kN - 1 || open_[size_t(i)] <= 0.0f;
}

std::span<const float> WaveField::heights() const {
    if (idle_ > kQuiet || !placed_) return {};
    return h_;
}

} // namespace bb::render
