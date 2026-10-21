// Sound effects of both games, simulated on the 30 Hz tick so voice state ('sound playing' conditions, the voice
// limit, chance rolls) stays deterministic; the app replays `cmds` on its AudioDevice. Spec + evidence:
// docs/formats/AUDIO_XA.md "Sound effects". LiT: table 0x41de40, request 0x41deb0, mixer 0x41edb0, levels 0x41f620,
// release-owned 0x41f5d0. TB: 0x406ff0 / 0x407080 / 0x42ae60 / 0x407250 / 0x407220.
#pragma once
#include "bb/audio.h"
#include "bb/game/litmath.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace bb::game {

// LiT tag 0x04 (8 B): {u16 id, u8 bank, u8 program, u16 flags, u8 add, u8 chance}; TB (12 B): add, chance and
// maxDist are u16. flags: 2 non-positional (levels = add), 0x10 released when the emitter is freed, 0x20 a looping
// voice follows its emitter, 0x80 chance (plays when rand() >= chance * 0x80), 0x100 emitter part offset (not
// applied), 0x200 add `add` to both levels, 0x400 speech (language table), TB 0x4000 maxDist replaces 0x1000.
struct SfxEntry {
    uint16_t id = 0;
    uint8_t bank = 0, program = 0;
    uint16_t flags = 0, add = 0, chance = 0, maxDist = 0;
};

// 0x41f620 / 0x407250 for a positioned emitter: {left (+0xc), right (+0xd)} levels 0..127. Distance (0x407970
// max + (mid + min) / 4) is measured from the camera target, direction is the emitter's angle from the eye minus the
// view angle (FPATAN · 2048/π, 4096 units; the exes' constants differ in the last digits); beyond maxDist both are 0.
std::array<uint8_t, 2> sfxLevels(const V3& src, const V3& eye, const V3& target, int32_t maxDist, bool tb);
// Start volume in DirectSound hundredths of dB from the levels, the program volume byte and the config SFX volume
// (LiT 0x9ca850 0..100, TB GD+0x97 0..127); TB tracks with the same formula.
int32_t sfxVolumeLit(std::array<uint8_t, 2> lv, uint8_t progVolume, int32_t config);
int32_t sfxVolumeTb(std::array<uint8_t, 2> lv, uint8_t progVolume, int32_t config);
inline int32_t sfxPan(std::array<uint8_t, 2> lv) { return (int32_t(lv[0]) - lv[1]) * 3000 >> 7; }
// LiT frequency: AIFF rate (always 44100) · 2^(5 − note/12), note = program byte 6 (the PS1 centre note); outside
// DirectSound's 100..100000 Hz SetFrequency fails and the buffer keeps its own rate (I).
uint32_t sfxRateLit(uint32_t rate, uint8_t note);

class Sfx {
public:
    bool tb = false;
    // false = the original without a sound device (the Wine oracle): the mixer never runs, so voices never free and
    // started voices stay 'start pending' (C85/C87 see them playing) — replays of oracle traces set this.
    bool device = true;
    int32_t volume = 100;  // config SFX volume: LiT 0..100, TB 0..127
    std::vector<SfxEntry> table;
    std::vector<std::array<uint8_t, 8>> programs, speechPrograms;  // {u16 blob (1-based), ?, ?, volume, ?, note, ?}
    std::shared_ptr<const std::vector<AiffSound>> bank, speech;    // section-3 bank / language speech bank
    struct Voice { uint16_t state = 0; int32_t emitter = 0; int16_t entry = -1; std::array<uint8_t, 2> lv{}; };
    std::array<Voice, 24> voices{};  // 0x4b3260 / 0x63b740: 0x8000 used, 1 start, 2 release, 4 playing
    std::vector<SfxCmd> cmds;        // device calls of the mixer passes since the app last drained them

    // Chunk-0 tag 0x04 (TB 12-B form when tb); the exes stop at 0x80 / 0xa2 entries.
    void addEntry(std::span<const uint8_t> tag);
    // Program table at a tag-0x01 / speech-tag {off, size} region: u32 n, n × 16 programs × 8 B.
    static std::vector<std::array<uint8_t, 8>> programTable(std::span<const uint8_t> region);
    // Every blob of a sfxBank-style table decoded (failed blobs stay empty = no buffer).
    static std::shared_ptr<const std::vector<AiffSound>> loadBank(std::span<const uint8_t> section, BzeVariant game);

    // Emitter: a world handle, 0 = none. The callback answers true for a player (levels fixed 100/100), else fills
    // the emitter's world position.
    using Where = std::function<bool(int32_t emitter, V3& pos)>;
    // 0x41deb0 / 0x407080: mode 1 start (needs a free voice, then the chance roll), mode 2 release.
    void request(int32_t emitter, uint16_t id, uint16_t mode, MsvcRand& rand);
    void releaseOwned(int32_t emitter);                  // 0x41f5d0 / 0x407220
    bool playing(uint16_t id, int32_t emitter) const;   // C85/C87, TB 0x35/0x37 (emitter 0 = any voice)
    // One mixer pass (0x41edb0 / 0x42ae60) heard from the camera eye looking at target.
    void mix(const V3& eye, const V3& target, const Where& where);

private:
    struct Copy { uint8_t kind = 0; uint32_t start = 0; uint64_t len = 0; uint32_t rate = 1; };  // 1 once, 2 looping
    struct Loop { uint16_t blob = 0; uint8_t pair = 0; int32_t emitter = 0; bool looping = false; };
    bool busy(const Copy& c) const { return c.kind == 2 || (c.kind == 1 && uint64_t(now_ - c.start) * c.rate < c.len); }
    std::array<uint8_t, 2> levels(const Voice& v, const SfxEntry& e, const Where& where, const V3& eye,
                                  const V3& target) const;
    const AiffSound* sound(uint32_t blob) const;
    void volumeAll(uint16_t blob, bool loop, int32_t vol);
    std::vector<std::array<Copy, 10>> copies_;  // per blob index: the exes' 10 DirectSound buffer copies
    std::vector<Loop> loops_;                    // 0x553200 / 0x5e56c0 looping instances (<= 10)
    uint32_t now_ = 0;                           // mixer passes
};

} // namespace bb::game
