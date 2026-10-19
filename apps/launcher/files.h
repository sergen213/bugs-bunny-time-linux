// Launcher file operations without Qt: install validation, Datas/ copy, save backup/restore.
// Original installs are only ever read.
#pragma once
#include "bb/scene.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bbtime {

struct InstallReport {
    std::filesystem::path root;
    std::optional<bb::GameId> game;
    std::string exePath, exeVersion, exeSha256;
    size_t files = 0, bzeFiles = 0, failures = 0;
    uint64_t bytes = 0;
    std::string datasDir; // relative name of the Datas directory as found (case varies), empty if absent
    bool ok(bb::GameId want) const { return game == want && !exePath.empty() && !datasDir.empty() && failures == 0; }
    std::string summary() const;
};

InstallReport checkInstall(const std::filesystem::path& root);

// Copies <src>/<datasDir> to <dst>/Datas via <dst>.partial, then swaps it in. `progress(done, total)` in bytes;
// setting `cancel` aborts and removes the partial copy. Returns an error message, empty on success.
std::string copyDatas(const InstallReport& src, const std::filesystem::path& dst,
                      const std::function<void(uint64_t, uint64_t)>& progress, const std::atomic<bool>& cancel);

// Copies saveDir() content to <backups>/<YYYYmmdd-HHMMSS>[suffix]/. Returns the backup dir or an error.
std::string backupSaves(const std::filesystem::path& saves, const std::filesystem::path& backups,
                        std::filesystem::path* out, const char* suffix = "");
// Backs up the current saves (suffix "-before-restore"), then replaces `saves` with `backup`.
std::string restoreSaves(const std::filesystem::path& backup, const std::filesystem::path& saves,
                         const std::filesystem::path& backups);
std::vector<std::filesystem::path> listBackups(const std::filesystem::path& backups); // newest first

} // namespace bbtime
