// Launcher file ops. `test_files` = synthetic temp-dir checks (save backup/restore, Datas copy, refusals);
// `test_files real` = both original installs under $BBTIME_DATA (exit 77 without it), read-only.
#include "files.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <print>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
void put(const fs::path& p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
std::string slurp(const fs::path& p) {
    std::ostringstream ss;
    ss << std::ifstream(p, std::ios::binary).rdbuf();
    return ss.str();
}

int real() {
    const char* data = std::getenv("BBTIME_DATA");
    if (!data || !*data) return 77;
    for (auto [key, id] : {std::pair{"lit", bb::GameId::LostInTime}, std::pair{"tb", bb::GameId::TimeBusters}}) {
        const bbtime::InstallReport r = bbtime::checkInstall(fs::path(data) / key);
        std::print("{}", r.summary());
        assert(r.ok(id) && r.exeSha256.size() == 64 && r.bzeFiles > 50 && r.files > 300);
        assert(!r.ok(id == bb::GameId::LostInTime ? bb::GameId::TimeBusters : bb::GameId::LostInTime));
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "real") return real();
    char tmpl[] = "/tmp/bbtime-files-XXXXXX";
    const fs::path t = mkdtemp(tmpl);

    // Datas copy from a read-only "disc": progress reaches the total, old copy replaced, source untouched.
    put(t / "disc/DATAS/bze/a.bze", "level");
    put(t / "disc/DATAS/track/b.xa", "music!");
    fs::permissions(t / "disc/DATAS/bze/a.bze", fs::perms(0444));
    bbtime::InstallReport src = bbtime::checkInstall(t / "disc");
    assert(src.datasDir == "DATAS" && !src.game && !src.ok(bb::GameId::LostInTime));
    put(t / "local/lit/Datas/stale.txt", "old");
    uint64_t last = 0, total = 0;
    std::atomic<bool> cancel = false;
    assert(bbtime::copyDatas(src, t / "local/lit", [&](uint64_t d, uint64_t n) { last = d, total = n; }, cancel).empty());
    assert(last == 11 && total == 11);
    assert(slurp(t / "local/lit/Datas/bze/a.bze") == "level" && !fs::exists(t / "local/lit/Datas/stale.txt"));
    assert(!fs::exists(t / "local/lit.partial") && !fs::exists(t / "local/lit.old"));
    assert(fs::status(t / "disc/DATAS/bze/a.bze").permissions() == fs::perms(0444));
    cancel = true;
    assert(bbtime::copyDatas(src, t / "local/tb", nullptr, cancel) == "cancelled" && !fs::exists(t / "local/tb.partial"));
    cancel = false;
    assert(bbtime::copyDatas(src, t / "disc/copy", nullptr, cancel) == "destination is inside the original install");
    assert(bbtime::copyDatas(src, t / "disc", nullptr, cancel) == "destination is inside the original install");

    // Saves: backup, modify, restore (current state is backed up first).
    const fs::path saves = t / "saves", backups = t / "backups";
    assert(!bbtime::backupSaves(saves, backups, nullptr).empty()); // nothing to back up
    put(saves / "lit/Savegame1.dat", "slot1-v1");
    fs::path b1;
    assert(bbtime::backupSaves(saves, backups, &b1).empty() && slurp(b1 / "lit/Savegame1.dat") == "slot1-v1");
    fs::path b2;
    assert(bbtime::backupSaves(saves, backups, &b2).empty() && b2 != b1); // same second -> distinct dir
    put(saves / "lit/Savegame1.dat", "slot1-v2");
    put(saves / "lit/Savegame2.dat", "slot2");
    assert(bbtime::restoreSaves(b1, saves, backups).empty());
    assert(slurp(saves / "lit/Savegame1.dat") == "slot1-v1" && !fs::exists(saves / "lit/Savegame2.dat"));
    const auto list = bbtime::listBackups(backups);
    assert(list.size() == 3);
    assert(std::ranges::count_if(list, [](const fs::path& p) { return p.filename().string().ends_with("-before-restore"); }) == 1);
    for (const auto& p : list)
        if (p.filename().string().ends_with("-before-restore")) assert(slurp(p / "lit/Savegame2.dat") == "slot2");
    assert(!bbtime::restoreSaves(t / "nope", saves, backups).empty());

    fs::permissions(t / "disc/DATAS/bze/a.bze", fs::perms(0644));
    fs::remove_all(t);
    std::println("files: all checks passed");
    return 0;
}
