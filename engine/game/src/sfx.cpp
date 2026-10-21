// Sound-effect voices of both games (bb/game/sfx.h; docs/formats/AUDIO_XA.md "Sound effects").
#include "bb/game/sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace bb::game {
namespace {

uint16_t u16(std::span<const uint8_t> d, size_t o) { return uint16_t(d[o] | d[o + 1] << 8); }

int32_t approxLen(int32_t x, int32_t y, int32_t z) {  // 0x407970 / TB 0x404060
    const int32_t a = std::abs(x), b = std::abs(y), c = std::abs(z), hi = std::max(a, b), lo = std::min(a, b);
    return hi > c ? hi + ((c + lo) >> 2) : c + ((hi + lo) >> 2);
}

}  // namespace

std::array<uint8_t, 2> sfxLevels(const V3& src, const V3& eye, const V3& target, int32_t maxDist, bool tb) {
    const int32_t d = approxLen(src.x - target.x, src.y - target.y, src.z - target.z);
    if (d >= maxDist) return {0, 0};
    // 0x4079e0 / 0x4040d0: FPATAN(x, z) · constant 0x45c270 / 0x4812e8, FISTP (round to nearest even).
    const double k = tb ? 651.8986469230359 : 651.89865;
    auto ang = [&](int32_t x, int32_t z) { return int32_t(std::lrint(std::atan2(double(x), double(z)) * k)); };
    int32_t a = ang(src.x - eye.x, src.z - eye.z) - ang(target.x - eye.x, target.z - eye.z);
    if (a > 0x800) a -= 0x1000;
    if (a < -0x800) a += 0x1000;
    const int32_t v = 0x7f - (d << 7) / maxDist;
    if (a < 0) {
        if (a < -0x400) a = -0x800 - a;
        return {uint8_t(v), uint8_t(v + (a * v >> 10))};
    }
    if (a > 0x400) a = 0x800 - a;
    return {uint8_t(v - (a * v >> 10)), uint8_t(v)};
}

int32_t sfxVolumeLit(std::array<uint8_t, 2> lv, uint8_t progVolume, int32_t config) {  // 0x41edb0
    const int32_t v = std::min(progVolume + 100, 0x7f);
    return ((v * (((lv[0] + lv[1]) >> 1) * config / 100) >> 7) - 0x7f) * 3000 >> 7;
}

int32_t sfxVolumeTb(std::array<uint8_t, 2> lv, uint8_t progVolume, int32_t config) {  // 0x42ae60
    if (config == 0 || (lv[0] | lv[1]) == 0) return -10000;
    const int32_t c = config * 100 >> 7, iv = int32_t(progVolume * 100u / 0x7f) * c / 100;
    return std::min(0, (((lv[0] + lv[1]) >> 1) * iv / 0x7f - 100) * 30);
}

uint32_t sfxRateLit(uint32_t rate, uint8_t note) {  // 0x41f04c: FLD 2.0, note · (1/12), 5 − that, pow, · rate, FISTP
    const long f = std::lrint(std::pow(2.0, 5.0 - note * (1.0 / 12.0)) * double(rate));
    return f < 100 || f > 100000 ? rate : uint32_t(f);
}

void Sfx::addEntry(std::span<const uint8_t> t) {
    if (t.size() < (tb ? 12u : 8u) || table.size() >= (tb ? 0xa2u : 0x80u)) return;
    SfxEntry e{u16(t, 0), t[2], t[3], u16(t, 4)};
    if (tb) e.add = u16(t, 6), e.chance = u16(t, 8), e.maxDist = u16(t, 10);
    else e.add = t[6], e.chance = t[7];
    table.push_back(e);
}

std::vector<std::array<uint8_t, 8>> Sfx::programTable(std::span<const uint8_t> r) {
    std::vector<std::array<uint8_t, 8>> out;
    if (r.size() < 4) return out;
    const size_t n = std::min<size_t>(r[0] | r[1] << 8 | r[2] << 16 | size_t(r[3]) << 24, (r.size() - 4) / 128);
    out.resize(n * 16);
    for (size_t i = 0; i < out.size(); ++i) std::copy_n(r.begin() + 4 + ptrdiff_t(i) * 8, 8, out[i].begin());
    return out;
}

std::shared_ptr<const std::vector<AiffSound>> Sfx::loadBank(std::span<const uint8_t> section, BzeVariant game) {
    auto out = std::make_shared<std::vector<AiffSound>>();
    for (std::span<const uint8_t> blob : sfxBank(section)) {
        try { out->push_back(parseAiff(blob, game)); } catch (const std::exception&) { out->emplace_back(); }
    }
    return out;
}

const AiffSound* Sfx::sound(uint32_t blob) const {
    // Buffer index = program blob (1-based); the speech bank follows the section-3 bank (base 0x4b1c80 / 0x4d0354).
    const size_t n = bank ? bank->size() : 0;
    const AiffSound* s = blob >= 1 && blob <= n ? &(*bank)[blob - 1]
                         : speech && blob > n && blob - n <= speech->size() ? &(*speech)[blob - n - 1]
                                                                            : nullptr;
    return s && !s->pcm.empty() ? s : nullptr;
}

void Sfx::request(int32_t emitter, uint16_t id, uint16_t mode, MsvcRand& rand) {
    if (mode & 1) {
        const auto v = std::ranges::find_if(voices, [](const Voice& x) { return !(x.state & 0x8000); });
        if (v == voices.end()) return;
        const auto e = std::ranges::find(table, id, &SfxEntry::id);
        if (e == table.end() || ((e->flags & 0x80) && rand() < int32_t(e->chance) * 0x80)) return;
        *v = {uint16_t(mode | 0x8000), emitter, int16_t(e - table.begin()), {}};
    } else if (mode & 2) {  // the emitter's own voice of that sound, else any voice of it
        auto of = [&](const Voice& x) { return (x.state & 0x8000) && table[size_t(x.entry)].id == id; };
        auto v = std::ranges::find_if(voices, [&](const Voice& x) { return of(x) && x.emitter == emitter; });
        if (v == voices.end()) v = std::ranges::find_if(voices, of);
        if (v != voices.end()) v->state = 0x8002;
    }
}

void Sfx::releaseOwned(int32_t emitter) {
    for (Voice& v : voices)
        if ((v.state & 0x8000) && v.emitter == emitter && (table[size_t(v.entry)].flags & 0x10))
            v.state = !tb && (v.state & 1) ? 0 : 0x8002;  // LiT drops a voice that never started
}

bool Sfx::playing(uint16_t id, int32_t emitter) const {
    return std::ranges::any_of(voices, [&](const Voice& v) {
        return (v.state & 0x8000) && (v.state & 5) && table[size_t(v.entry)].id == id && (!emitter || v.emitter == emitter);
    });
}

std::array<uint8_t, 2> Sfx::levels(const Voice& v, const SfxEntry& e, const Where& where, const V3& eye,
                                   const V3& target) const {
    V3 pos;
    if (where(v.emitter, pos)) return {100, 100};  // a player: 0x6464
    std::array<uint8_t, 2> lv = sfxLevels(pos, eye, target, tb && (e.flags & 0x4000) ? e.maxDist : 0x1000, tb);
    if (e.flags & 0x200)
        for (uint8_t& x : lv) x = uint8_t(std::min(x + e.add, 0x7f));
    return lv;
}

void Sfx::volumeAll(uint16_t blob, bool loop, int32_t vol) {  // SetVolume on the blob's 10 buffers
    const auto& cp = copies_[blob];
    for (uint8_t c = 0; c < 10; c += loop ? 2 : 1)
        if (cp[c].kind || (loop && cp[c + 1].kind)) cmds.push_back({SfxCmd::Volume, blob * 10u + c, nullptr, 0, vol, 0});
}

void Sfx::mix(const V3& eye, const V3& target, const Where& where) {
    if (!device) return;
    ++now_;
    // Looping instances: once the first buffer passes the loop start the second loops on (0x41edb0 head).
    for (Loop& l : loops_) {
        Copy& a = copies_[l.blob][l.pair];
        const AiffSound* s = sound(l.blob);
        if (l.looping || !s || uint64_t(now_ - a.start) * a.rate < uint64_t(s->loopStart) * 30) continue;
        a.kind = 0, copies_[l.blob][l.pair + 1].kind = 2, l.looping = true;
    }
    for (Voice& v : voices) {
        if (!(v.state & 0x8000)) continue;
        const SfxEntry& e = table[size_t(v.entry)];
        const auto& progs = (e.flags & 0x400) ? speechPrograms : programs;
        const size_t pi = size_t(e.bank) * 16 + e.program;
        const std::array<uint8_t, 8> prog = pi < progs.size() ? progs[pi] : std::array<uint8_t, 8>{};
        const uint32_t blob = uint32_t(prog[0] | prog[1] << 8) + ((e.flags & 0x400) && bank ? bank->size() : 0);
        if (blob >= 0x80) continue;  // both exes skip it (the voice stays)
        const AiffSound* s = sound(blob);
        if (!s) {
            if (tb) v.state = 0;  // LiT leaves the voice in place
            continue;
        }
        if (copies_.size() <= blob) copies_.resize(blob + 1);
        auto& cp = copies_[blob];
        const bool loop = s->loopEnd > 0;
        if (v.state & 1) {  // start (the request's view index always matches with one view)
            if (!v.emitter) { v.state = 0; continue; }
            v.state = uint16_t((v.state & ~1u) | 4);
            v.lv = (e.flags & 2) ? std::array{uint8_t(e.add), uint8_t(e.add)} : levels(v, e, where, eye, target);
            const int32_t vol = tb ? sfxVolumeTb(v.lv, prog[4], volume) : sfxVolumeLit(v.lv, prog[4], volume);
            const uint32_t rate = tb ? s->rate : sfxRateLit(s->rate, prog[6]);
            uint8_t c = 0;
            if (!loop) {
                while (c < 10 && busy(cp[c])) ++c;
            } else {
                while (c < 10 && (busy(cp[c]) || cp[c + 1].kind == 2)) c += 2;
                if (c < 10 && (loops_.size() >= 10 || std::ranges::any_of(loops_, [&](const Loop& l) {
                                   return l.blob == blob && l.emitter == v.emitter;
                               })))
                    c = 10;  // one instance per blob and emitter
                if (c < 10) loops_.push_back({uint16_t(blob), c, v.emitter, false});
            }
            if (c == 10) continue;
            cp[c] = {1, now_, uint64_t(s->frames()) * 30, rate};
            cmds.push_back({SfxCmd::Play, blob * 10u + c, s, rate, vol, sfxPan(v.lv)});
        } else if (v.state & 2) {  // release
            if (!loop) {
                for (uint8_t c = 0; c < 10; ++c)
                    if (std::exchange(cp[c].kind, 0)) cmds.push_back({SfxCmd::Stop, blob * 10u + c});
            } else if (const auto l = std::ranges::find_if(loops_, [&](const Loop& x) {
                           return x.blob == blob && x.emitter == v.emitter;
                       });
                       l != loops_.end()) {
                cp[l->pair].kind = cp[l->pair + 1].kind = 0;
                cmds.push_back({SfxCmd::Stop, blob * 10u + l->pair});
                loops_.erase(l);
            }
            v.state = 0;
        } else if (v.state & 4) {
            if (!loop) {
                if (!busy(cp[0])) v.state = 0;  // the exes poll buffer copy 0 only
            } else if (e.flags & 0x20) {        // follow the emitter
                if (tb) {
                    if (!v.emitter) { v.state |= 2; continue; }
                    v.lv = levels(v, e, where, eye, target);
                    volumeAll(uint16_t(blob), true, sfxVolumeTb(v.lv, prog[4], volume));
                    v.state |= 1;  // TB re-requests the start: a no-op while its instance loops
                } else {
                    if (v.emitter) v.lv = levels(v, e, where, eye, target);
                    if (v.emitter && (v.lv[0] | v.lv[1])) volumeAll(uint16_t(blob), true, sfxVolumeLit(v.lv, prog[4], volume));
                    else v.state = 2;  // voice freed; the instance keeps looping at its last volume until released
                }
            }
        }
    }
}

} // namespace bb::game
