#include "bb/manifest.h"
#include "bb/levelscript.h"
#include "bb/lzss.h"

#include <openssl/evp.h>

#include <cctype>
#include <algorithm>
#include <cstring>
#include <format>
#include <memory>

namespace bb {
namespace {

std::string digest(const EVP_MD* md, std::span<const uint8_t> data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    if (!EVP_Digest(data.data(), data.size(), out, &len, md, nullptr)) throw std::runtime_error("EVP_Digest failed");
    std::string hex;
    for (unsigned i = 0; i < len; ++i) hex += std::format("{:02x}", out[i]);
    return hex;
}

struct KnownExe {
    const char* hash;  // md5 for LiT (BugsDecomp version table), sha256 for TB
    bool md5;
    GameId game;
    const char* version;
};
constexpr KnownExe kKnownExes[] = {
    {"59731f0354f8a66e63502bd3b521bd82", true, GameId::LostInTime, "LiT 1.0"},
    {"121db63d76ed5077d69837b111163cac", true, GameId::LostInTime, "LiT 1.6"},
    {"63483ea5205b8982e384e0b1863624cb8df4fce1601d0cd74393e5b69ecf78c1", false, GameId::TimeBusters,
     "TB EN ISO, unknown patch level"},
};

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

bool starts(std::span<const uint8_t> b, size_t off, std::string_view magic) {
    return b.size() >= off + magic.size() && std::memcmp(b.data() + off, magic.data(), magic.size()) == 0;
}

std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

// Format by content. BZE has no magic; first word 0/1 + nonzero section count + valid 0x7fc checksum
// is the signature (headerless XA files start with >= 0x800 zero bytes, which also checksum to 0).
std::string sniff(std::span<const uint8_t> b) {
    if (starts(b, 0, "MZ")) {
        const uint32_t pe = rd32(b, 0x3c);
        if (pe + 24 <= b.size() && starts(b, pe, std::string_view("PE\0\0", 4))) {
            const uint16_t characteristics = uint16_t(b[pe + 22] | b[pe + 23] << 8);
            return characteristics & 0x2000 ? "dll-pe" : "exe-pe";
        }
        return "exe-mz";
    }
    if (starts(b, 0, "BM") && rd32(b, 2) == b.size()) return "bmp";
    if (starts(b, 0, "RIFF") && starts(b, 8, "WAVE")) return "wav";
    if (starts(b, 0, "RIFF") && starts(b, 8, "AVI ")) return "avi";
    if (starts(b, 4, "ftyp")) return "mp4";
    if (starts(b, 0, "MSCF")) return "cab";
    if (starts(b, 0, "ISc(")) return "cab-is";
    if (starts(b, 0, "%PDF")) return "pdf";
    if (starts(b, 0, std::string_view("\0\1\0\0", 4)) || starts(b, 0, "true") || starts(b, 0, "OTTO")) return "ttf";
    if (b.size() >= BzeFile::kHeaderSize && rd32(b, 0) <= 1 && rd32(b, rd32(b, 0) == 1 ? 4 : 8) != 0 &&
        rd32(b, BzeFile::kChecksumOffset) == bzeChecksum(b))
        return "bze";
    return {};
}

void inspectBze(ManifestFile& mf, std::vector<uint8_t> bytes) {
    BzeFile bze = parseBze(std::move(bytes));
    mf.bzeVariant = bze.variant;
    mf.tbBudget = bze.tbBudget;
    for (size_t i = 0; i < bze.sections.size(); ++i) {
        ManifestSection& ms = mf.sections.emplace_back(ManifestSection{.section = bze.sections[i]});
        ms.compressed = bze.variant == BzeVariant::LostInTime;
        std::vector<uint8_t> payload;
        try {
            payload = bze.payload(i);
            ms.decodedSize = payload.size();
        } catch (const FormatError& e) {
            ms.error = e.what();
            continue;
        }
        if (ms.section.id != 1) continue;
        try {
            ms.scriptItems = uint32_t(parseLevelScript(payload, bze.variant).items.size());
        } catch (const FormatError& e) {
            ms.scriptError = e.what();
        }
    }
}

void jsonString(std::string& o, std::string_view s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += {'\\', char(c)};
        else if (c < 0x20) o += std::format("\\u{:04x}", c);
        else o += char(c);  // ponytail: assumes UTF-8 paths; raw non-UTF-8 bytes would make invalid JSON
    }
    o += '"';
}

} // namespace

Manifest scanInstall(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    Manifest m;
    m.root = fs::absolute(root);
    for (const auto& de : fs::recursive_directory_iterator(root)) {
        if (!de.is_regular_file()) continue;
        ManifestFile& mf = m.files.emplace_back();
        mf.path = fs::relative(de.path(), root).generic_string();
        try {
            std::vector<uint8_t> bytes = readFile(de.path());
            mf.size = bytes.size();
            mf.sha256 = digest(EVP_sha256(), bytes);
            mf.format = sniff(bytes);
            mf.byMagic = !mf.format.empty();
            if (!mf.byMagic) {
                const std::string ext = lowerExt(de.path());
                mf.format = ext == ".xa" ? "xa" : ext == ".bze" ? "bze" : "unknown";
            }
            if (mf.format == "exe-pe" || mf.format == "dll-pe") mf.md5 = digest(EVP_md5(), bytes);
            if (mf.format == "bze") inspectBze(mf, std::move(bytes));
        } catch (const std::exception& e) {
            mf.error = e.what();
        }
    }
    std::ranges::sort(m.files, {}, &ManifestFile::path);
    for (const ManifestFile& f : m.files)
        for (const KnownExe& k : kKnownExes)
            if ((k.md5 ? f.md5 : f.sha256) == k.hash) {
                m.game = k.game;
                m.exePath = f.path;
                m.exeVersion = k.version;
            }
    if (!m.game) {  // no known exe hash: fall back to the BZE header variant
        for (const ManifestFile& f : m.files)
            if (f.bzeVariant) {
                m.game = *f.bzeVariant == BzeVariant::LostInTime ? GameId::LostInTime : GameId::TimeBusters;
                break;
            }
    }
    return m;
}

std::string manifestJson(const Manifest& m) {
    std::string o;
    auto key = [&](std::string_view k) { jsonString(o, k); o += ':'; };
    auto str = [&](std::string_view k, std::string_view v) { key(k); jsonString(o, v); o += ','; };
    auto num = [&](std::string_view k, uint64_t v) { key(k); o += std::format("{},", v); };
    auto close = [&](char c) { if (o.back() == ',') o.back() = c; else o += c; };

    o += '{';
    str("schema", "bbtime-manifest/1");
    str("root", m.root.string());
    str("game", !m.game ? "unknown" : *m.game == GameId::LostInTime ? "LostInTime" : "TimeBusters");
    str("exe", m.exePath);
    str("exeVersion", m.exeVersion.empty() ? "unknown" : m.exeVersion);
    size_t bzeCount = 0, bzeFailed = 0, sectionFailed = 0, scriptOk = 0, scriptFailed = 0;
    uint64_t total = 0;
    for (const ManifestFile& f : m.files) {
        total += f.size;
        if (f.format != "bze") continue;
        ++bzeCount;
        bzeFailed += !f.error.empty();
        for (const ManifestSection& s : f.sections) {
            sectionFailed += !s.error.empty();
            if (s.section.id == 1 && s.error.empty()) (s.scriptItems ? scriptOk : scriptFailed)++;
        }
    }
    num("fileCount", m.files.size());
    num("totalBytes", total);
    key("summary");
    o += std::format(R"({{"bze":{},"bzeFailed":{},"sectionsFailed":{},"levelScriptsOk":{},"levelScriptsFailed":{}}},)",
                     bzeCount, bzeFailed, sectionFailed, scriptOk, scriptFailed);
    key("files");
    o += '[';
    for (const ManifestFile& f : m.files) {
        o += '{';
        str("path", f.path);
        num("size", f.size);
        str("sha256", f.sha256);
        if (!f.md5.empty()) str("md5", f.md5);
        str("format", f.format);
        str("formatEvidence", f.byMagic ? "magic" : "extension");
        if (!f.error.empty()) str("error", f.error);
        if (f.bzeVariant) {
            key("bze");
            o += '{';
            str("variant", *f.bzeVariant == BzeVariant::LostInTime ? "LostInTime" : "TimeBusters");
            if (*f.bzeVariant == BzeVariant::TimeBusters) num("tbBudget", f.tbBudget);
            key("sections");
            o += '[';
            for (const ManifestSection& s : f.sections) {
                o += '{';
                num("id", s.section.id);
                num("offset", s.section.offset);
                num("actualSize", s.section.actualSize);
                num("fileSize", s.section.fileSize);
                str("compression", s.compressed ? "lzss" : "none");
                if (s.error.empty()) num("decodedSize", s.decodedSize);
                else str("error", s.error);
                if (s.scriptItems) num("levelScriptItems", *s.scriptItems);
                if (!s.scriptError.empty()) str("levelScriptError", s.scriptError);
                close('}');
                o += ',';
            }
            close(']');
            o += "},";
        }
        close('}');
        o += ',';
    }
    close(']');
    o += "}\n";
    return o;
}

} // namespace bb
