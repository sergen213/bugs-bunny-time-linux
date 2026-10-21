// OpenAL playback: streamed .xa music + one-shot AIFF sound effects, pumped from the main loop.
#include "bb/audio.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <stdexcept>

namespace bb {
namespace {
// ~0.25 s per buffer, 4 buffers: the main loop must pump at least every ~0.75 s.
constexpr size_t kChunkFrames = kXaRate / 4;
// DirectSound volume / pan unit: hundredths of a dB of attenuation; -10000 = silence.
float dsGain(int32_t v) { return v <= -10000 ? 0.0f : std::pow(10.0f, float(v) / 2000.0f); }
void drop(unsigned src, unsigned buf) {
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
}
} // namespace

AudioDevice::AudioDevice() {
    dev_ = alcOpenDevice(nullptr);
    if (!dev_) return;
    ctx_ = alcCreateContext(dev_, nullptr);
    if (!ctx_ || !alcMakeContextCurrent(ctx_)) {
        if (ctx_) alcDestroyContext(ctx_);
        ctx_ = nullptr;
        alcCloseDevice(dev_);
        dev_ = nullptr;
        return;
    }
    alGenSources(1, &musicSrc_);
    alGenBuffers(kBuffers, musicBufs_);
}

AudioDevice::~AudioDevice() {
    if (!ctx_) return;
    stopMusic();
    for (Voice v : sfx_) drop(v.src, v.buf);
    stopSfx();
    alDeleteSources(1, &musicSrc_);
    alDeleteBuffers(kBuffers, musicBufs_);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(ctx_);
    alcCloseDevice(dev_);
}

void AudioDevice::queue(unsigned buffer) {
    scratch_.clear();
    const size_t bytes = music_->layout() == XaLayout::Stereo ? kChunkFrames : kChunkFrames / 2;
    size_t got = music_->read(bytes, scratch_);
    if (got < bytes && loop_ && music_->bytes() > 0) {
        music_->rewind();
        music_->read(bytes - got, scratch_);
    }
    if (scratch_.empty()) return;
    alBufferData(buffer, music_->layout() == XaLayout::Stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 scratch_.data(), ALsizei(scratch_.size() * sizeof(int16_t)), ALsizei(kXaRate));
    alSourceQueueBuffers(musicSrc_, 1, &buffer);
}

void AudioDevice::playMusic(const std::filesystem::path& xa, bool loop) {
    if (!ctx_) return;
    stopMusic();
    music_.emplace(xa);
    loop_ = loop;
    for (unsigned b : musicBufs_) queue(b);
    alSourcef(musicSrc_, AL_GAIN, musicGain_);
    if (!paused_) alSourcePlay(musicSrc_);
}

void AudioDevice::stopMusic() {
    if (!ctx_) return;
    alSourceStop(musicSrc_);
    alSourcei(musicSrc_, AL_BUFFER, 0);  // unqueues everything
    music_.reset();
}

void AudioDevice::pauseMusic(bool paused) {
    if (!ctx_ || paused == paused_) return;
    paused_ = paused;
    if (!music_) return;
    if (paused) alSourcePause(musicSrc_);
    else alSourcePlay(musicSrc_);
}

void AudioDevice::setMusicVolume(float gain) {
    musicGain_ = std::clamp(gain, 0.0f, 1.0f);
    if (ctx_) alSourcef(musicSrc_, AL_GAIN, musicGain_);
}

void AudioDevice::setSfxVolume(float gain) { sfxGain_ = std::clamp(gain, 0.0f, 1.0f); }

void AudioDevice::playSfx(const AiffSound& s) {
    if (!ctx_ || s.pcm.empty()) return;
    Voice v{};
    alGenSources(1, &v.src);
    alGenBuffers(1, &v.buf);
    alBufferData(v.buf, s.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, s.pcm.data(),
                 ALsizei(s.pcm.size() * sizeof(int16_t)), ALsizei(s.rate));
    alSourcei(v.src, AL_BUFFER, ALint(v.buf));
    alSourcef(v.src, AL_GAIN, sfxGain_);
    alSourcePlay(v.src);
    sfx_.push_back(v);
}

// DSBPAN on mono and stereo buffers alike: pan > 0 attenuates the left side by pan, pan < 0 the right by -pan. The pan
// is baked into a stereo copy (direct channels) so OpenAL's own panning never applies; a looping sound repeats
// [loopStart, loopEnd) with -pan, as the exes play that region from a second buffer set up with the negated pan.
void AudioDevice::sfx(const SfxCmd& c) {
    if (!ctx_) return;
    const auto it = keyed_.find(c.key);
    if (c.op == SfxCmd::Volume) {
        if (it != keyed_.end()) alSourcef(it->second.src, AL_GAIN, dsGain(c.volume));
        return;
    }
    if (it != keyed_.end()) {
        drop(it->second.src, it->second.buf);
        keyed_.erase(it);
    }
    if (c.op == SfxCmd::Stop || !c.sound || c.sound->pcm.empty()) return;
    const AiffSound& s = *c.sound;
    const bool loop = s.loopEnd > 0;
    const uint32_t frames = loop ? s.loopEnd : s.frames();
    auto sides = [](int32_t pan) { return std::array{pan > 0 ? dsGain(-pan) : 1.0f, pan < 0 ? dsGain(pan) : 1.0f}; };
    const auto g = sides(c.pan), gl = sides(-c.pan);
    scratch_.resize(size_t(frames) * 2);
    for (size_t f = 0; f < frames; ++f) {
        const auto& k = loop && f >= s.loopStart ? gl : g;
        for (size_t ch = 0; ch < 2; ++ch)
            scratch_[f * 2 + ch] = int16_t(std::lround(s.pcm[f * s.channels + (s.channels == 2 ? ch : 0)] * k[ch]));
    }
    Voice v{};
    alGenSources(1, &v.src);
    alGenBuffers(1, &v.buf);
    alBufferData(v.buf, AL_FORMAT_STEREO16, scratch_.data(), ALsizei(scratch_.size() * sizeof(int16_t)), ALsizei(c.rate));
    if (loop) {
        const ALint pts[2] = {ALint(s.loopStart), ALint(s.loopEnd)};
        alBufferiv(v.buf, AL_LOOP_POINTS_SOFT, pts);  // without AL_SOFT_loop_points the whole buffer loops
        alSourcei(v.src, AL_LOOPING, AL_TRUE);
    }
    alSourcei(v.src, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
    alSourcei(v.src, AL_BUFFER, ALint(v.buf));
    alSourcef(v.src, AL_GAIN, dsGain(c.volume));
    alSourcePlay(v.src);
    keyed_[c.key] = v;
}

void AudioDevice::stopSfx() {
    for (auto [k, v] : keyed_) drop(v.src, v.buf);
    keyed_.clear();
}

void AudioDevice::update() {
    if (!ctx_) return;
    auto finished = [](Voice v) {
        ALint st = 0;
        alGetSourcei(v.src, AL_SOURCE_STATE, &st);
        if (st == AL_PLAYING) return false;
        drop(v.src, v.buf);
        return true;
    };
    std::erase_if(sfx_, finished);
    std::erase_if(keyed_, [&](const auto& kv) { return finished(kv.second); });
    if (!music_) return;
    ALint done = 0, queued = 0, state = 0;
    alGetSourcei(musicSrc_, AL_BUFFERS_PROCESSED, &done);
    while (done-- > 0) {
        ALuint b = 0;
        alSourceUnqueueBuffers(musicSrc_, 1, &b);
        queue(b);
    }
    alGetSourcei(musicSrc_, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(musicSrc_, AL_SOURCE_STATE, &state);
    if (queued == 0) music_.reset();                    // non-looping track finished
    else if (state != AL_PLAYING && !paused_) alSourcePlay(musicSrc_);  // recover from an underrun (late pump)
}

} // namespace bb
