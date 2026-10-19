// Synthetic BZE headers (both variants, corrupt/truncated) and level-script streams.
#include "bb/bze.h"
#include "bb/levelscript.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <vector>

using Bytes = std::vector<uint8_t>;

static void put32(Bytes& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }

static void seal(Bytes& b) { put32(b, 0x7fc, bb::bzeChecksum(b)); }

struct Sec { uint32_t id, actual, file; };

static Bytes build(bool tb, const std::vector<Sec>& secs) {
    size_t total = 0x800;
    for (auto& s : secs) total += s.file;
    Bytes b(total, 0);
    size_t e = tb ? 12 : 8;
    put32(b, 0, tb ? 0 : 1);
    if (tb) put32(b, 4, 222000);
    put32(b, tb ? 8 : 4, uint32_t(secs.size()));
    for (auto& s : secs) {
        put32(b, e, s.id), put32(b, e + 4, s.actual), put32(b, e + 8, s.file);
        e += 12;
    }
    for (size_t i = 0x800; i < total; ++i) b[i] = uint8_t(i * 7);
    b[0x100] = 0xff;  // a negative byte exercises the signed checksum
    seal(b);
    return b;
}

static bool bzeThrows(Bytes b, bool reseal = true) {
    if (reseal && b.size() >= 0x800) seal(b);
    try {
        bb::parseBze(std::move(b));
    } catch (const bb::FormatError&) {
        return true;
    }
    return false;
}

static bool scriptThrows(const Bytes& s, bb::BzeVariant v = bb::BzeVariant::LostInTime) {
    try {
        bb::parseLevelScript(s, v);
    } catch (const bb::FormatError&) {
        return true;
    }
    return false;
}

int main() {
    // --- container ---
    const Bytes lit = build(false, {{1, 0x123, 0x800}, {3, 0x1000, 0x1000}});
    {
        const bb::BzeFile f = bb::parseBze(lit);
        assert(f.variant == bb::BzeVariant::LostInTime && f.sections.size() == 2);
        assert(f.sections[0].offset == 0x800 && f.sections[1].offset == 0x1000 && f.sections[1].id == 3);
        assert(f.raw(0).size() == 0x123 && f.raw(0)[0] == uint8_t(0x800 * 7) && f.find(3) == 1 && f.find(4) == -1);
    }
    {
        const bb::BzeFile f = bb::parseBze(build(true, {{1, 0x10, 0x800}, {3, 0, 0x800}, {4, 0x7ff, 0x800}}));
        assert(f.variant == bb::BzeVariant::TimeBusters && f.tbBudget == 222000 && f.sections.size() == 3);
        assert(f.payload(2).size() == 0x7ff);  // TB sections are stored raw
    }
    {
        Bytes b = lit;
        b[0x7fc] ^= 1;
        assert(bzeThrows(b, false));  // checksum mismatch
    }
    assert(bzeThrows(Bytes(lit.begin(), lit.begin() + 0x7ff), false));  // truncated header
    assert(bzeThrows(Bytes(lit.begin(), lit.end() - 0x800)));          // truncated sections
    {
        Bytes b = lit;
        b.resize(b.size() + 0x800);
        assert(bzeThrows(b));  // trailing data
    }
    auto mutated = [&](size_t off, uint32_t v) {
        Bytes b = lit;
        put32(b, off, v);
        return b;
    };
    assert(bzeThrows(mutated(0, 2)));           // unknown first word
    assert(bzeThrows(mutated(4, 0)));           // zero sections
    assert(bzeThrows(mutated(4, 0xffffffff)));  // count overruns the header
    assert(bzeThrows(mutated(8 + 4, 0x801)));   // actual > file
    assert(bzeThrows(mutated(8 + 8, 0x7ff)));   // file size not sector-aligned
    assert(bzeThrows(mutated(8 + 8, 0xfffff800)));  // section runs past EOF (no 32-bit wrap)

    // --- level script (LiT tables) ---
    Bytes s = {0x2d, 0x45, 0x00, 0x45, 0x07, 0x2e,  // pseudo-chunk with two 45 tags
               0x2c,                                  // command
               0x2d, 0x07,                            // entity chunk
               0x10, 0x00, 0x10, 0, 0, 0x00, 0xe0, 0xff, 0xff, 0x00, 0x20, 0, 0,  // pos (1, -2, 2)
               0x11, 0x00, 0x04, 0, 0, 0, 0,          // rot (0x400 = 90 deg, 0, 0)
               0x13, 0x01,                            // player
               0x27, 0x2a, 0x00, 0, 0,                // entity id 42
               0x2e, 0x4b, 0x2f};
    {
        const bb::LevelScript ls = bb::parseLevelScript(s);
        assert(ls.items.size() == 5 && ls.end == s.size());
        assert(ls.items[0].kind == bb::ScriptItem::Kind::PseudoChunk && ls.items[0].tags.size() == 2);
        const bb::ScriptItem& e = ls.items[2];
        assert(e.kind == bb::ScriptItem::Kind::Chunk && e.code == 0x07 && e.offset == 7 && e.tags.size() == 4);
        assert(e.position && (*e.position)[0] == 0x1000 && (*e.position)[1] == -0x2000 && (*e.position)[2] == 0x2000);
        assert(e.rotation && (*e.rotation)[0] == 0x400 && e.player && e.entityId == 42 && !e.scale);
        assert(ls.items[3].kind == bb::ScriptItem::Kind::Command && ls.items[4].kind == bb::ScriptItem::Kind::End);
    }
    {
        Bytes bad = s;
        bad[29] = 0x99;  // tag 0x13 -> unknown tag in chunk type 07
        try {
            bb::parseLevelScript(bad);
            assert(false);
        } catch (const bb::FormatError& e) {
            assert(std::strstr(e.what(), "unknown tag 0x99") && std::strstr(e.what(), "offset 0x1d"));
        }
    }
    assert(scriptThrows(Bytes(s.begin(), s.end() - 1)));   // missing end
    assert(scriptThrows(Bytes(s.begin(), s.begin() + 12)));  // truncated tag operand
    assert(scriptThrows({0x2d, 0x99, 0x2e, 0x2f}));         // unknown chunk type
    assert(scriptThrows({0x55, 0x2f}));                     // unknown command
    // TB: structural bytes shifted by -1; no TB tag tables yet, so only empty scripts parse.
    assert(bb::parseLevelScript(Bytes{0x2e}, bb::BzeVariant::TimeBusters).end == 1);
    assert(scriptThrows({0x2c, 0x3a, 0x2d, 0x2e}, bb::BzeVariant::TimeBusters));
    return 0;
}
