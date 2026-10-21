// Audio for both games: headerless IMA-ADPCM `.xa` streams, in-BZE AIFF sound effects, OpenAL playback.
// Spec + evidence: docs/formats/AUDIO_XA.md.
#pragma once
#include "bb/bze.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <map>
#include <span>
#include <vector>

struct ALCdevice;
struct ALCcontext;

namespace bb {

constexpr uint32_t kXaRate = 44100;

// One IMA/DVI ADPCM channel (bugs.exe CvtStereoAdpcm 0x401be0). Starts at sample 0, index 0.
struct ImaState {
    int16_t sample = 0;
    uint8_t index = 0;
    int16_t decode(uint8_t nibble);
};

// Stereo: 1 byte = 1 frame, low nibble left, high nibble right (music, LiT speech).
// Mono: 1 byte = 2 frames, low nibble first (TB Datas/SPEECH/NN/*.xa).
enum class XaLayout : uint8_t { Stereo, Mono };

inline int xaChannels(XaLayout l) { return l == XaLayout::Stereo ? 2 : 1; }
inline uint64_t xaFrames(uint64_t bytes, XaLayout l) { return l == XaLayout::Stereo ? bytes : bytes * 2; }
// TB speech (…/SPEECH/<NN>/*.xa) is mono; every other .xa of both games is stereo.
XaLayout xaLayoutFor(const std::filesystem::path& p);

// Decodes every byte of `in`, appending interleaved s16 samples. `st` holds one state per channel.
void decodeXa(std::span<const uint8_t> in, XaLayout layout, ImaState (&st)[2], std::vector<int16_t>& out);

// File-backed streaming decoder. Throws std::runtime_error if the file cannot be opened.
class XaStream {
public:
    XaStream(const std::filesystem::path& p, XaLayout layout);
    explicit XaStream(const std::filesystem::path& p) : XaStream(p, xaLayoutFor(p)) {}
    // Appends up to maxBytes bytes' worth of decoded samples to `out`; returns bytes consumed (0 at end).
    size_t read(size_t maxBytes, std::vector<int16_t>& out);
    void rewind();  // seek to 0 and reset predictor state, like the game's loop restart
    XaLayout layout() const { return layout_; }
    uint64_t bytes() const { return bytes_; }
    uint64_t frames() const { return xaFrames(bytes_, layout_); }

private:
    std::ifstream f_;
    XaLayout layout_;
    uint64_t bytes_ = 0;
    ImaState st_[2]{};
    std::vector<uint8_t> buf_;
};

// AIFF sound effect (big-endian FORM/AIFF, COMM + SSND; other chunks skipped), 1–2 channels.
// LostInTime: 8/16-bit big-endian PCM. TimeBusters: 16-bit COMM but SSND holds 4-bit IMA ADPCM decoded like
// .xa (bblit2 0x42a7c0 → CvtMonoAdpcm 0x40ded0 / CvtStereoAdpcm 0x40dc90, state 0); length = SSND size.
struct AiffSound {
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    bool truncated = false;    // COMM frame count exceeded the SSND data; pcm holds what exists
    std::vector<int16_t> pcm;  // interleaved, native endian
    // Loop region in frames (loopEnd 0 = one-shot). Needs >= 2 MARK markers: LiT (0x41e950) takes the positions of
    // the INST sustain-loop begin/end marker ids, TB (0x42a7c0) the first two markers in MARK order.
    uint32_t loopStart = 0, loopEnd = 0;
    uint32_t frames() const { return channels ? uint32_t(pcm.size() / channels) : 0; }
};
// Throws FormatError (bb/bze.h) for malformed / truncated input.
AiffSound parseAiff(std::span<const uint8_t> in, BzeVariant game = BzeVariant::LostInTime);

// Section-3 sound bank (both games): u32 n, u32 offsets[n] -> FORM/AIFF blobs. Returns each blob (bounded by
// its FORM size), or empty if the section does not start with such a table (cutscene/loader levels start with TIMs).
std::vector<std::span<const uint8_t>> sfxBank(std::span<const uint8_t> section3);

// One DirectSound buffer call of a game's SFX mixer (bb::game::Sfx), replayed by AudioDevice::sfx. `key` names a
// buffer copy (the exes keep 10 per blob). Volume in hundredths of dB (-10000 silent .. 0); pan -10000 (left only)
// .. 10000 (right only), DSBPAN: the other side is attenuated by |pan| hundredths of dB.
struct SfxCmd {
    enum Op : uint8_t { Play, Volume, Stop } op = Play;
    uint32_t key = 0;
    const AiffSound* sound = nullptr;  // Play: a looping sound plays [0, loopStart) then repeats [loopStart, loopEnd)
                                       // with the pan negated (the exes' second buffer gets -pan)
    uint32_t rate = 0;                 // Play: buffer frequency (Hz)
    int32_t volume = 0, pan = 0;       // Play / Volume (pan: Play only)
};

// OpenAL output. Pump update() once per frame; there is no audio thread.
class AudioDevice {
public:
    AudioDevice();  // opens the default device; ok() is false if none
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    bool ok() const { return ctx_ != nullptr; }
    // Streams an .xa file (layout from xaLayoutFor), restarting from byte 0 at the end when loop is set.
    // Throws std::runtime_error if the file cannot be opened.
    void playMusic(const std::filesystem::path& xa, bool loop = true);
    void stopMusic();
    bool musicPlaying() const { return music_.has_value(); }
    void pauseMusic(bool paused);     // hold / resume the stream at its position (no-op when unchanged)
    void setMusicVolume(float gain);  // 0..1
    void setSfxVolume(float gain);    // 0..1: playSfx gain; games read it as their config SFX volume (sfxVolume())
    float sfxVolume() const { return sfxGain_; }
    void playSfx(const AiffSound& s);  // non-positional one-shot at the sfx volume (UI sounds)
    void sfx(const SfxCmd& c);         // game mixer command (bb::game::Sfx::cmds)
    void stopSfx();                    // stop every mixer voice (level change)
    void update();                     // refill music buffers, reap finished sound effects

private:
    static constexpr int kBuffers = 4;
    void queue(unsigned buffer);

    ALCdevice* dev_ = nullptr;
    ALCcontext* ctx_ = nullptr;
    unsigned musicSrc_ = 0;
    unsigned musicBufs_[kBuffers]{};
    std::optional<XaStream> music_;
    bool loop_ = true, paused_ = false;
    float musicGain_ = 1.0f, sfxGain_ = 1.0f;
    std::vector<int16_t> scratch_;
    struct Voice { unsigned src, buf; };
    std::vector<Voice> sfx_;
    std::map<uint32_t, Voice> keyed_;  // mixer voices by SfxCmd::key
};

} // namespace bb
