// SFX voices (bb/game/sfx.h): levels / volume / pitch against values evaluated by hand from the exe code
// (0x41f620, 0x407250, 0x41edb0, 0x42ae60), the voice machine (start, poll copy 0, loop follow, release, voice
// limit), and the real banks when BBTIME_DATA is set.
#include "bb/game/lit_world.h"
#include "bb/game/sfx.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace bb;
using namespace bb::game;

namespace {

using Lv = std::array<uint8_t, 2>;

void testFormulas() {
    const V3 eye{0, 0, 0}, tgt{0, 0, 1000};
    // At the target: d 0, angle 0 -> full both sides.
    assert((sfxLevels(tgt, eye, tgt, 0x1000, false) == Lv{127, 127}));
    // (1000, 0, 1000): d = 1000 + (0 + 0) / 4, v = 127 - 128000 / 4096 = 96; angle atan2(1000, 1000) = 512 ->
    // left 96 - (512 * 96 >> 10) = 48. Pan (48 - 96) * 3000 >> 7 = -1125.
    assert((sfxLevels({1000, 0, 1000}, eye, tgt, 0x1000, false) == Lv{48, 96}));
    assert(sfxPan({48, 96}) == -1125);
    // (-1000, 0, 0): d = 1000 + (1000 + 0) / 4 = 1250, v = 127 - 39 = 88; angle -1024 -> right 88 - 88 = 0.
    assert((sfxLevels({-1000, 0, 0}, eye, tgt, 0x1000, false) == Lv{88, 0}));
    // Behind (0, 0, -1000): d 2000, v 65; angle 0x800 folds to 0 -> both 65.
    assert((sfxLevels({0, 0, -1000}, eye, tgt, 0x1000, false) == Lv{65, 65}));
    // Out of range (d 5000 >= 4096) -> silent levels.
    assert((sfxLevels({5000, 0, 1000}, eye, tgt, 0x1000, false) == Lv{0, 0}));
    // TB maxDist 2000: v = 127 - (1000 << 7) / 2000 = 63, left 63 - (512 * 63 >> 10) = 32.
    assert((sfxLevels({1000, 0, 1000}, eye, tgt, 2000, true) == Lv{32, 63}));

    // LiT 0x41edb0: ((min(prog + 100, 127) * (avg * cfg / 100) >> 7) - 127) * 3000 >> 7.
    assert(sfxVolumeLit({100, 100}, 127, 100) == -657);  // Bugs (0x6464)
    assert(sfxVolumeLit({127, 127}, 40, 100) == -24);
    assert(sfxVolumeLit({0, 0}, 127, 100) == -2977);     // out of range still plays at about -30 dB
    assert(sfxVolumeLit({127, 127}, 127, 50) == -1524);
    // TB 0x42ae60: ((avg * ((prog * 100 / 127) * (cfg * 100 >> 7) / 100) / 127) - 100) * 30, <= 0; silent cases.
    assert(sfxVolumeTb({127, 127}, 70, 127) == -1380);
    assert(sfxVolumeTb({100, 100}, 127, 127) == -690);
    assert(sfxVolumeTb({0, 0}, 127, 127) == -10000);
    assert(sfxVolumeTb({127, 127}, 127, 0) == -10000);
    // LiT pitch 2^(5 - note / 12): note 60 native, 78 (most programs) 15592 Hz, 84 11025; out of DirectSound range kept.
    assert(sfxRateLit(44100, 60) == 44100);
    assert(sfxRateLit(44100, 78) == 15592);
    assert(sfxRateLit(44100, 84) == 11025);
    assert(sfxRateLit(44100, 0) == 44100);
}

Sfx synthetic(bool loop) {
    Sfx s;
    const uint8_t e1[8] = {5, 0, 0, 0, 0x20, 0, 0, 0};     // id 5, bank 0 program 0, flags 0x20
    const uint8_t e2[8] = {6, 0, 0, 0, 0x80, 0, 0, 0x80};  // id 6, chance byte 0x80
    s.addEntry(e1);
    s.addEntry(e2);
    s.programs.assign(16, {});
    s.programs[0] = {1, 0, 0, 0, 127, 0, 60, 0};  // blob 1, volume 127, note 60
    auto bank = std::make_shared<std::vector<AiffSound>>(1);
    AiffSound& a = (*bank)[0];
    a.channels = 1, a.bits = 16, a.rate = 3000;   // 300 frames = 0.1 s = 3 ticks
    a.pcm.assign(300, 1000);
    if (loop) a.loopStart = 100, a.loopEnd = 300;
    s.bank = bank;
    return s;
}

void testVoices() {
    const V3 eye{0, 0, 0}, tgt{0, 0, 1000};
    MsvcRand rand;
    auto where = [](int32_t e, V3& p) {
        if (e == 1) return true;
        p = e == 7 ? V3{1000, 0, 1000} : V3{9000, 0, 0};
        return false;
    };
    {  // one-shot: start pending -> playing -> freed when copy 0 ends
        Sfx s = synthetic(false);
        s.request(7, 5, 1, rand);
        assert(s.voices[0].state == 0x8001 && s.playing(5, 0) && s.playing(5, 7) && !s.playing(5, 8));
        s.mix(eye, tgt, where);
        assert(s.voices[0].state == 0x8004 && s.cmds.size() == 1);
        const SfxCmd& c = s.cmds[0];
        assert(c.op == SfxCmd::Play && c.key == 10 && c.rate == 3000 && c.volume == -1313 && c.pan == -1125);
        s.mix(eye, tgt, where);
        s.mix(eye, tgt, where);
        assert(s.voices[0].state == 0x8004);
        s.mix(eye, tgt, where);
        assert(s.voices[0].state == 0 && !s.playing(5, 0));
    }
    {  // without a device: 24 voices fill and never free; the 25th request finds none and skips the chance roll
        Sfx s = synthetic(false);
        s.device = false;
        for (int i = 0; i < 24; ++i) s.request(1, 5, 1, rand);
        const uint32_t seed = rand.seed;
        s.request(1, 6, 1, rand);
        assert(rand.seed == seed && !s.playing(6, 0));
        s.mix(eye, tgt, where);
        assert(s.cmds.empty() && s.voices[23].state == 0x8001);
        s.request(1, 5, 2, rand);  // release marks the first voice; 0x8002 no longer counts as playing for C85
        assert(s.voices[0].state == 0x8002);
    }
    {  // loop with flag 0x20: follows the emitter; out of range frees the voice (LiT) but the instance keeps looping
        Sfx s = synthetic(true);
        s.request(7, 5, 1, rand);
        s.mix(eye, tgt, where);
        assert(s.cmds.size() == 1 && s.cmds[0].op == SfxCmd::Play);
        s.cmds.clear();
        s.mix(eye, tgt, where);  // tracking: same levels, volume to the instance
        assert(s.cmds.size() == 1 && s.cmds[0].op == SfxCmd::Volume && s.cmds[0].volume == -1313);
        s.cmds.clear();
        s.mix(eye, tgt, [](int32_t, V3& p) { p = {9000, 0, 0}; return false; });
        assert(!(s.voices[0].state & 0x8000) && s.cmds.empty());
        s.request(7, 5, 1, rand);  // same emitter again: no second instance, tracking resumes
        s.mix(eye, tgt, where);
        assert(s.cmds.empty() && s.voices[0].state == 0x8004);
        s.request(7, 5, 2, rand);
        s.mix(eye, tgt, where);
        assert(s.cmds.size() == 1 && s.cmds[0].op == SfxCmd::Stop && s.cmds[0].key == 10 && s.voices[0].state == 0);
    }
    {  // non-positional (flags 2) and Bugs (0x6464)
        Sfx s = synthetic(false);
        s.table[0].flags = 2, s.table[0].add = 90;
        s.request(7, 5, 1, rand);
        s.request(1, 6, 1, rand);  // chance 0x80 * 0x80 = 0x4000: plays when rand() >= 16384
        s.mix(eye, tgt, where);
        assert(s.voices[0].lv == (Lv{90, 90}) && s.cmds[0].pan == 0);
    }
}

void testRealBanks(const std::filesystem::path& data) {
    const auto lit = data / "lit/Datas/bze/L01a.bze";
    if (std::filesystem::exists(lit)) {
        const BzeFile bze = loadBze(lit);
        const LitWorld w = LitWorld::load(bze, nullptr);
        assert(w.sfx.bank && w.sfx.bank->size() == 53 && w.sfx.speech && !w.sfx.speech->empty());
        assert(!w.sfx.table.empty() && !w.sfx.programs.empty() && !w.sfx.speechPrograms.empty());
        size_t loops = 0;
        for (const AiffSound& a : *w.sfx.bank) loops += a.loopEnd > 0;
        std::printf("L01a: %zu sounds, %zu bank blobs (%zu looping), %zu speech blobs\n", w.sfx.table.size(),
                    w.sfx.bank->size(), loops, w.sfx.speech->size());
        // Oracle local/wine/sfxcheck/lit_starts.csv (RUNNER=wine, /b04, hook at the start path's SetFrequency
        // 0x41f094): every captured sound's frequency = the first entry's program note through sfxRateLit.
        const std::pair<uint16_t, uint32_t> freq[] = {{3, 15592}, {7, 13891}, {8, 15592}, {9, 12375}, {16, 15592},
                                                      {18, 15592}, {53, 15592}, {69, 15592}, {70, 15592}, {71, 13891},
                                                      {72, 13891}, {95, 24750}, {96, 23361}, {103, 15592}};
        for (auto [id, hz] : freq) {
            const auto e = std::ranges::find(w.sfx.table, id, &SfxEntry::id);
            assert(e != w.sfx.table.end());
            const auto& p = (e->flags & 0x400 ? w.sfx.speechPrograms : w.sfx.programs)[size_t(e->bank) * 16 + e->program];
            const size_t blob = size_t(p[0] | p[1] << 8) - 1;
            const uint32_t rate = e->flags & 0x400 ? (*w.sfx.speech)[blob].rate : (*w.sfx.bank)[blob].rate;
            assert(sfxRateLit(rate, p[6]) == hz);
        }
    }
    {  // same capture: levels stored at voice +0xc/+0xd at the start vs emitter, eye, target read in the hook
        struct Row { V3 em, eye, tgt; Lv lv; };
        const Row rows[] = {
            {{24246, 437, 12389}, {23613, -1000, 11550}, {23700, -1039, 9655}, {26, 14}},  // id 69 @919
            {{24246, 437, 12389}, {22224, -1347, 10279}, {23700, -843, 9655}, {28, 6}},    // 70 @1097
            {{24246, 437, 12389}, {23182, -1347, 9826}, {24813, -843, 9510}, {23, 2}},     // 71 @1357
            {{24246, 437, 12389}, {24377, -1347, 9576}, {25962, -843, 9272}, {7, 1}},      // 72 @1437
            {{24246, 437, 12389}, {23700, -1140, 8481}, {23700, -843, 9655}, {26, 28}},    // 70 @1695
            {{24246, 437, 12389}, {24845, -1140, 9545}, {23757, -843, 9514}, {3, 24}},     // 71 @1795
            {{24246, 437, 12389}, {24058, -1140, 8493}, {25421, -843, 9213}, {9, 3}},      // 70 @2293
            {{23984, -624, 15130}, {22260, -1396, 10342}, {23700, -895, 9655}, {0, 0}},    // 95 @1021
        };
        for (const Row& r : rows) assert(sfxLevels(r.em, r.eye, r.tgt, 0x1000, false) == r.lv);
    }
    const auto tb = data / "tb/Datas/BZE/ARCA01.bze";
    if (std::filesystem::exists(tb)) {
        const BzeFile bze = loadBze(tb);
        const auto bank = Sfx::loadBank(bze.payload(size_t(bze.find(3))), BzeVariant::TimeBusters);
        assert(bank->size() == 51);
        size_t loops = 0;
        for (const AiffSound& a : *bank) loops += a.loopEnd > 0;
        std::printf("ARCA01: %zu bank blobs (%zu looping)\n", bank->size(), loops);
    }
}

} // namespace

int main() {
    testFormulas();
    testVoices();
    if (const char* d = std::getenv("BBTIME_DATA")) testRealBanks(d);
    std::puts("sfx ok");
}
