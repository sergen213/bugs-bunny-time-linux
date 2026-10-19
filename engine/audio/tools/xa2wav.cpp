// xa2wav IN.xa OUT.wav — decode a LiT/TB .xa stream to 16-bit PCM WAV (layout from the path, see audio.h).
#include "bb/audio.h"

#include <cstdio>
#include <fstream>
#include <print>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: xa2wav IN.xa OUT.wav");
        return 2;
    }
    try {
        bb::XaStream xa(argv[1]);
        std::vector<int16_t> pcm;
        while (xa.read(1 << 20, pcm) > 0) {}
        const uint32_t ch = uint32_t(bb::xaChannels(xa.layout())), data = uint32_t(pcm.size() * 2);
        auto u32 = [](uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
        auto u16 = [](uint32_t v) { return std::string{char(v), char(v >> 8)}; };
        std::ofstream out(argv[2], std::ios::binary);
        out << "RIFF" << u32(36 + data) << "WAVEfmt " << u32(16) << u16(1) << u16(ch) << u32(bb::kXaRate)
            << u32(bb::kXaRate * ch * 2) << u16(ch * 2) << u16(16) << "data" << u32(data);
        out.write(reinterpret_cast<const char*>(pcm.data()), data);
        if (!out) throw std::runtime_error(std::string("cannot write ") + argv[2]);
        std::println("{}: {} bytes, {} ch -> {} frames, {:.3f} s", argv[1], xa.bytes(), ch, pcm.size() / ch,
                     double(pcm.size() / ch) / bb::kXaRate);
    } catch (const std::exception& e) {
        std::println(stderr, "xa2wav: {}", e.what());
        return 1;
    }
    return 0;
}
