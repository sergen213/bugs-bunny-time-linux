// Time Busters blob shadows and floor shade: the draw-list pass of 0x465130 (shade steps), 0x464480 (shadow list)
// and 0x464090 (placement on the floor below). RUNTIME_REFERENCE.md §35 (blob shadows / floor shade).
#include "bb/game/tb_world.h"

namespace bb::game {

// 0x464090: false = no floor within 2250 below. Position x / z: the world origin (0x44e560) of the model instance's
// head coordinate, its first part with an object (the hips: Bugs' TOD part 1, oracle rootpeek_arca01.txt), else +0xfc.
bool TbWorld::shadowPlace(TbActor& a, Shadow& s) {
    const auto head = std::ranges::find_if(a.anim.parts, [](const TodPart& p) { return (p.flags & 4) && !p.dead; });
    s.pos = head == a.anim.parts.end() ? a.pos : partWorld(a, int32_t(head - a.anim.parts.begin()));
    // Standing on a platform (+0x90 with flags1 0x200000): the shadow lies on it at the feet, turned as it.
    if (const TbActor* p = entity(a.parent); p && !(a.f24 & 0x200) && (p->flags1 & 0x200000) && !(a.flags10 & 0x2000000)) {
        s.pos.y = a.pos.y, s.rot = {p->pitch, p->yaw, p->roll};
        return true;
    }
    std::array<int32_t, 3> n{0, -0x4000, 0};  // floor normal (Q14)
    int32_t surf = -1;                        // floor surface record, -1 none
    if (!(a.f20 & 2) || (a.f28 & 2) || a.groundPoly < 0 || (a.flags10 & 0x2000000)) {
        // Ray 2250 down from 100 above the feet (query 0x105: polygons, and the shapes of flags1 0x200000 entities).
        const V3 st{a.pos.x, a.pos.y - 100, a.pos.z};
        V3 d{0, 0x8ca, 0};
        std::array<int16_t, 3> hn{};
        const auto h = rayHit(a, st, d, 5, 0x105, {0, 0x200000, 0, 0, 0, 0}, true, {}, &hn);
        if (!h) return false;
        s.pos.y = d.y + a.pos.y - 100;
        n = {hn[0], hn[1], hn[2]};  // a polygon's or an entity shape's (its surface stays none: no shade)
        if (!*h)
            if (const auto ph = col.query(st, {0, 0x8ca, 0}, 5, 0x105)) surf = col.polys[size_t(ph->poly)].surf;
    } else {  // on the floor polygon +0x108 (surface +0x144)
        s.pos.y = a.pos.y - 4;
        const TbPoly& gp = col.polys[size_t(a.groundPoly)];
        n = {gp.n[0], gp.n[1], gp.n[2]}, surf = a.groundSurf;
    }
    if (a.flags10 & 0x200) {  // 0x464400: the floor surface's flag 0x80 shades the entity and its attached children
        auto shadeFrom = [&](auto& self, TbActor& e, uint32_t f) -> void {
            if (f & 0x80) {
                e.shadeTarget = 0x10, e.f28 |= 0x1000;
                if (e.progMask[2] & 0x1000) runProgram(e, 0, 0, 0);
            } else {
                e.shadeTarget = 0, e.f28 &= ~0x1000u;
            }
            forEachEntity([&](TbActor& c) { if (c.parent == int32_t(e.uid)) self(self, c, f); });
        };
        if (surf >= 0 && size_t(surf) < col.surfaces.size()) shadeFrom(shadeFrom, a, col.surfaces[size_t(surf)].flags);
    } else {
        a.f28 &= ~0x1000u;
    }
    const int32_t z = ratan2(n[0], n[1]);
    s.rot = {int16_t(-ratan2(n[2], isqrt(n[0] * n[0] + n[1] * n[1]))), a.yaw, int16_t(0x800 - z)};
    // The view's 0x4d515c & 0x100 would keep a floor above the feet; 0x465130 clears it before this pass.
    if (s.pos.y < a.pos.y) s.pos.y = a.pos.y;
    return true;
}

// 0x465130's draw loop (shade steps, the shadow list of up to 40) then 0x464480 over that list.
void TbWorld::shadowPass() {
    shadows.clear();
    std::vector<TbActor*> list;
    for (TbActor* a : order()) {
        if (!a->active || (a->f24 & 4) || !a->drawn() || a->curAnim == 0xf6 || a->curAnim == 0x4f || (a->flags10 & 0x200000))
            continue;
        if (a->behaviour == 7 || (a->flags1 & 0x2000000)) continue;
        if (a->shadowId && list.size() < 0x28 && a->model) list.push_back(a);
        if (a->f28 & 0x1000) {
            if (a->shade < a->shadeTarget) a->shade = uint8_t(a->shade + 2);
            if (a->shadeTarget < a->shade) a->shade = a->shadeTarget;
        } else if (a->shade) {
            a->shade = a->shade < 3 ? 0 : uint8_t(a->shade - 2);
        }
    }
    for (TbActor* a : list) {
        if (a->f24 & 0x40) continue;
        const auto m = shadowModels_.find(a->shadowId);
        Shadow s;
        if (!shadowPlace(*a, s) || m == shadowModels_.end()) continue;
        s.model = m->second.get(), s.uid = a->uid;
        s.scale = std::max((a->pos.y - s.pos.y) * 2 + 0x1000, 0x800);
        shadows.push_back(s);
    }
}

} // namespace bb::game
