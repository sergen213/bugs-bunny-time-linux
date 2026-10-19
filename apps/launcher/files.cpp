#include "files.h"

#include "bb/manifest.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace fs = std::filesystem;

namespace bbtime {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

bool isEmptyDir(const fs::path& p) {
    std::error_code ec;
    return !fs::is_directory(p, ec) || fs::is_empty(p, ec);
}

// Replaces `dst` with `partial` (rename; the old copy is removed afterwards).
std::string swapIn(const fs::path& partial, const fs::path& dst) {
    std::error_code ec;
    const fs::path old = dst.string() + ".old";
    fs::remove_all(old, ec);
    if (fs::exists(dst, ec)) fs::rename(dst, old, ec);
    if (!ec) fs::rename(partial, dst, ec);
    if (ec) return std::format("cannot move {} into place: {}", dst.string(), ec.message());
    fs::remove_all(old, ec);
    return {};
}

} // namespace

std::string InstallReport::summary() const {
    std::string s = std::format("{}\n  game: {}\n", root.string(),
                                !game ? "not detected" : *game == bb::GameId::LostInTime ? "Bugs Bunny: Lost in Time"
                                                                                          : "Bugs Bunny & Taz: Time Busters");
    s += std::format("  executable: {} ({})\n  exe SHA-256: {}\n", exePath.empty() ? "not found" : exePath,
                     exeVersion.empty() ? "unknown version" : exeVersion, exeSha256.empty() ? "-" : exeSha256);
    s += std::format("  files: {} ({:.1f} MiB), BZE levels: {}, decode failures: {}\n", files, double(bytes) / (1 << 20),
                     bzeFiles, failures);
    s += std::format("  data directory: {}\n", datasDir.empty() ? "missing Datas/" : datasDir);
    return s;
}

InstallReport checkInstall(const fs::path& root) {
    InstallReport r;
    r.root = root;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return r;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && iequals(e.path().filename().string(), "Datas")) r.datasDir = e.path().filename().string();
    bb::Manifest m;
    try {
        m = bb::scanInstall(root);
    } catch (const std::exception&) {
        r.failures = 1;
        return r;
    }
    r.game = m.game;
    r.exePath = m.exePath;
    r.exeVersion = m.exeVersion;
    r.files = m.files.size();
    for (const bb::ManifestFile& f : m.files) {
        r.bytes += f.size;
        r.bzeFiles += f.format == "bze";
        if (f.path == m.exePath) r.exeSha256 = f.sha256;
        if (!f.error.empty() || std::ranges::any_of(f.sections, [](auto& s) { return !s.error.empty(); })) ++r.failures;
    }
    return r;
}

std::string copyDatas(const InstallReport& src, const fs::path& dst,
                      const std::function<void(uint64_t, uint64_t)>& progress, const std::atomic<bool>& cancel) {
    if (src.datasDir.empty()) return "source has no Datas/ directory";
    std::error_code ec;
    const fs::path from = fs::canonical(src.root / src.datasDir, ec);
    if (ec) return std::format("cannot open {}", (src.root / src.datasDir).string());
    const fs::path to = fs::weakly_canonical(dst, ec);
    const fs::path rel = to.lexically_relative(fs::canonical(src.root, ec));
    if (!rel.empty() && *rel.begin() != "..") return "destination is inside the original install";

    uint64_t total = 0, done = 0;
    for (const auto& e : fs::recursive_directory_iterator(from, ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    const fs::path partial = dst.string() + ".partial";
    fs::remove_all(partial, ec);
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (cancel) {
            fs::remove_all(partial, ec);
            return "cancelled";
        }
        const fs::path out = partial / "Datas" / it->path().lexically_relative(from);
        if (it->is_directory(ec)) fs::create_directories(out, ec);
        else if (it->is_regular_file(ec)) {
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(it->path(), out, ec);
            fs::permissions(out, fs::perms::owner_write, fs::perm_options::add, ec); // disc files are read-only
            done += it->file_size(ec);
            if (progress) progress(done, total);
        }
        if (ec) break;
    }
    if (ec) {
        fs::remove_all(partial, ec);
        return std::format("copy failed: {}", ec.message());
    }
    return swapIn(partial, dst);
}

std::string backupSaves(const fs::path& saves, const fs::path& backups, fs::path* out, const char* suffix) {
    if (isEmptyDir(saves)) return std::format("no saves in {}", saves.string());
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    fs::path dir = backups / std::format("{:%Y%m%d-%H%M%S}Z{}", now, suffix);
    std::error_code ec;
    for (int n = 2; fs::exists(dir, ec); ++n) dir = backups / std::format("{:%Y%m%d-%H%M%S}Z{}-{}", now, suffix, n);
    fs::create_directories(dir, ec);
    fs::copy(saves, dir, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(dir, ec);
        return std::format("backup failed: {}", ec.message());
    }
    if (out) *out = dir;
    return {};
}

std::string restoreSaves(const fs::path& backup, const fs::path& saves, const fs::path& backups) {
    if (!fs::is_directory(backup)) return std::format("{} is not a backup", backup.string());
    if (!isEmptyDir(saves))
        if (std::string e = backupSaves(saves, backups, nullptr, "-before-restore"); !e.empty()) return e;
    std::error_code ec;
    const fs::path partial = saves.string() + ".partial";
    fs::remove_all(partial, ec);
    fs::create_directories(partial, ec);
    fs::copy(backup, partial, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(partial, ec);
        return std::format("restore failed: {}", ec.message());
    }
    return swapIn(partial, saves);
}

std::vector<fs::path> listBackups(const fs::path& backups) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(backups, ec))
        if (e.is_directory(ec)) v.push_back(e.path());
    std::ranges::sort(v, std::greater{});
    return v;
}

} // namespace bbtime
