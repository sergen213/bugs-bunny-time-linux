// OpenAL playback: streamed .xa music + one-shot AIFF sound effects, pumped from the main loop.
#include "bb/audio.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <stdexcept>

namespace bb {
namespace {
// ~0.25 s per buffer, 4 buffers: the main loop must pump at least every ~0.75 s.
constexpr size_t kChunkFrames = kXaRate / 4;
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
    for (Voice v : sfx_) {
        alDeleteSources(1, &v.src);
        alDeleteBuffers(1, &v.buf);
    }
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
    alSourcePlay(musicSrc_);
}

void AudioDevice::stopMusic() {
    if (!ctx_) return;
    alSourceStop(musicSrc_);
    alSourcei(musicSrc_, AL_BUFFER, 0);  // unqueues everything
    music_.reset();
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

void AudioDevice::update() {
    if (!ctx_) return;
    std::erase_if(sfx_, [](Voice v) {
        ALint st = 0;
        alGetSourcei(v.src, AL_SOURCE_STATE, &st);
        if (st == AL_PLAYING) return false;
        alDeleteSources(1, &v.src);
        alDeleteBuffers(1, &v.buf);
        return true;
    });
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
    else if (state != AL_PLAYING) alSourcePlay(musicSrc_);  // recover from an underrun (late pump)
}

} // namespace bb
