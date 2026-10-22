// Signed updater (no Qt): HTTPS-only fetch, Ed25519 manifest signature (libsodium), per-artifact SHA-256,
// atomic versioned install with symlink swap, rollback, downgrade refusal. Scheme: docs/UPDATES.md.
// Only ever writes below Options::installRoot; game data and original installs are never touched.
#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bbtime::update {

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
std::optional<Version> parseVersion(std::string_view s); // strict "X.Y.Z"

struct Artifact {
    std::string path;   // relative install path, [A-Za-z0-9._-] components, no "..", no leading "."
    std::string url;    // https://
    uint64_t size = 0;
    std::string sha256; // lowercase hex
    bool executable = false;
};

struct Manifest {
    std::string version, notes;
    std::vector<Artifact> artifacts;
};

using PublicKey = std::array<unsigned char, 32>;
std::optional<PublicKey> decodePublicKey(std::string_view base64);
PublicKey releasePublicKey(); // tools/release/bbtime-update.pub, baked in at build time

// Verifies the base64 detached Ed25519 signature over the exact manifest bytes, then parses and validates the
// JSON (schema 1, product "bbtime", version, artifact fields/paths/URLs).
std::expected<Manifest, std::string> verifyManifest(std::string_view json, std::string_view sigBase64, const PublicKey& key);

struct Options {
    std::filesystem::path installRoot;  // holds versions/<X.Y.Z>/, current -> versions/..., previous -> ...
    PublicKey key = releasePublicKey();
    std::string caFile;                 // extra trust anchor (tests); empty = system store
    bool force = false;                 // allow reinstall/downgrade
    std::function<void(uint64_t done, uint64_t total)> progress;
};

// HTTPS GET into memory, refusing bodies larger than maxBytes. Redirects must stay on https.
std::expected<std::string, std::string> fetch(const std::string& url, const Options& o, size_t maxBytes);

// Fetches `manifestUrl` and `manifestUrl + ".sig"` and verifies them.
std::expected<Manifest, std::string> check(const std::string& manifestUrl, const Options& o);

std::optional<std::string> currentVersion(const std::filesystem::path& installRoot);  // target of `current`
std::optional<std::string> previousVersion(const std::filesystem::path& installRoot);

// Release channel: the latest GitHub release's signed manifest. settings.ini `update_url` overrides it (tests/dev).
inline constexpr const char* kDefaultManifestUrl =
    "https://github.com/sergen213/bugs-bunny-time-linux/releases/latest/download/manifest.json";
// An override into this repository's own releases (e.g. a pinned .../releases/download/v0.8.0/manifest.json that the
// pre-0.9 launcher made users type) means "our releases": it follows the latest release instead of freezing a version.
inline std::string manifestUrl(std::string_view override) {
    constexpr std::string_view ownReleases = "https://github.com/sergen213/bugs-bunny-time-linux/releases/";
    return override.empty() || override.starts_with(ownReleases) ? kDefaultManifestUrl : std::string(override);
}

// The release AppImage, installed as <installRoot>/current/bbtime-launcher; the launcher restarts into it.
inline constexpr const char* kLauncherArtifact = "bbtime-launcher";
inline constexpr const char* kHandoffEnv = "BBTIME_LAUNCHER_HANDOFF"; // set by a launcher that handed off: no further hop

// The installed launcher to hand off to on start: current/bbtime-launcher when it is executable and `current` is newer
// than `running`. Empty otherwise (no install, no launcher artifact, same or older version).
std::filesystem::path handoffTarget(const std::filesystem::path& installRoot, std::string_view running);

// An updater-installed artifact (current/<name>, e.g. "bbtime") to run instead of the copy bundled with the running
// launcher: only when it is executable and `current` is at least as new as `running`. Empty otherwise, so a newer
// AppImage never runs an older installed game.
std::filesystem::path installedArtifact(const std::filesystem::path& installRoot, std::string_view running,
                                        std::string_view name);

// Refuses unless m.version > max(current, runningVersion) or o.force. Downloads every artifact into a staging
// directory, verifies size + SHA-256, renames it to versions/<v>, then swaps `current` (old one becomes
// `previous`). Returns the installed version directory.
std::expected<std::filesystem::path, std::string> install(const Manifest& m, const Options& o, std::string_view runningVersion);

// Swaps `current` and `previous`. Returns the version now current.
std::expected<std::string, std::string> rollback(const std::filesystem::path& installRoot);

} // namespace bbtime::update
