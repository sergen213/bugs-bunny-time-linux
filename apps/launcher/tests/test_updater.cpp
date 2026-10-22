// Updater paths against a local throwaway HTTPS server (tests/https_server.py): signature valid / invalid /
// tampered manifest, tampered and oversized artifacts, non-https refusal, downgrade refusal, forced downgrade,
// rollback, path traversal, and Python release scripts -> libsodium verify compatibility.
#include "updater.h"

#include <sodium.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace bbtime::update;

namespace {

fs::path tmp;
std::string base; // https://127.0.0.1:PORT
unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];

void put(const fs::path& p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
std::string slurp(const fs::path& p) {
    std::ostringstream ss;
    ss << std::ifstream(p, std::ios::binary).rdbuf();
    return ss.str();
}
std::string sha256(std::string_view s) {
    unsigned char d[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(d, reinterpret_cast<const unsigned char*>(s.data()), s.size());
    char hex[65];
    return sodium_bin2hex(hex, sizeof hex, d, sizeof d);
}
std::string sign(std::string_view msg, const unsigned char* key) {
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), key);
    char b64[sodium_base64_ENCODED_LEN(crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL)];
    return sodium_bin2base64(b64, sizeof b64, sig, sizeof sig, sodium_base64_VARIANT_ORIGINAL);
}

// Publishes release `v` (files served under /v/) and returns its manifest URL. `artifactPath` overrides the first
// artifact's install path; `declaredSha`/`declaredSize` override what the manifest claims.
std::string publish(std::string v, std::string game = "", std::string artifactPath = "bbtime",
                    std::string declaredSha = "", int64_t declaredSize = -1) {
    if (game.empty()) game = "game binary " + v;
    const std::string notes = "data for " + v;
    put(tmp / "srv" / v / "bbtime", game);
    put(tmp / "srv" / v / "notes.txt", notes);
    const std::string json = std::format(
        R"({{"schema":1,"product":"bbtime","version":"{0}","notes":"release {0}","artifacts":[)"
        R"({{"path":"{1}","url":"{2}/{0}/bbtime","size":{3},"sha256":"{4}","executable":true}},)"
        R"({{"path":"share/notes.txt","url":"{2}/{0}/notes.txt","size":{5},"sha256":"{6}"}}]}})",
        v, artifactPath, base, declaredSize >= 0 ? uint64_t(declaredSize) : game.size(),
        declaredSha.empty() ? sha256(game) : declaredSha, notes.size(), sha256(notes));
    put(tmp / "srv" / v / "manifest.json", json);
    put(tmp / "srv" / v / "manifest.json.sig", sign(json, sk));
    return std::format("{}/{}/manifest.json", base, v);
}

int run(const std::string& cmd) { return std::system(cmd.c_str()); }

} // namespace

int main() {
    if (sodium_init() < 0) return 1;
    char tmpl[] = "/tmp/bbtime-updater-XXXXXX";
    tmp = mkdtemp(tmpl);
    fs::create_directories(tmp / "srv");
    crypto_sign_keypair(pk, sk);

    // Server: stdin pipe keeps it alive; stdout pipe yields the port.
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execlp("python3", "python3", BBTIME_HTTPS_SERVER, (tmp / "srv").c_str(), (tmp / "ca.pem").c_str(), nullptr);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    char portBuf[16] = {};
    if (read(out[0], portBuf, sizeof portBuf - 1) <= 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        fs::remove_all(tmp);
        std::println("https_server.py unavailable (exit {}) - skipping", WEXITSTATUS(st));
        return 77;
    }
    base = std::format("https://127.0.0.1:{}", std::atoi(portBuf));

    Options o;
    o.installRoot = tmp / "app";
    std::ranges::copy(pk, o.key.begin());
    o.caFile = (tmp / "ca.pem").string();
    uint64_t lastProgress = 0;
    o.progress = [&](uint64_t done, uint64_t) { lastProgress = done; };

    // Valid manifest.
    const std::string u020 = publish("0.2.0");
    auto m = check(u020, o);
    assert(m && m->version == "0.2.0" && m->artifacts.size() == 2 && m->notes == "release 0.2.0");

    // Transport: https only, and only trusted certificates.
    assert(!check("http://127.0.0.1:1/x/manifest.json", o));
    assert(check("file://" + (tmp / "srv/0.2.0/manifest.json").string(), o).error().starts_with("refusing non-https"));
    Options untrusted = o;
    untrusted.caFile.clear();
    assert(!check(u020, untrusted));

    // Signature: wrong key, tampered manifest bytes, garbage signature.
    unsigned char pk2[crypto_sign_PUBLICKEYBYTES], sk2[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk2, sk2);
    const std::string json = slurp(tmp / "srv/0.2.0/manifest.json");
    assert(verifyManifest(json, sign(json, sk), o.key));
    assert(verifyManifest(json, sign(json, sk2), o.key).error() == "manifest signature does not verify");
    assert(!verifyManifest(json + " ", sign(json, sk), o.key));
    std::string evil = json;
    evil.replace(evil.find("0.2.0"), 5, "9.2.0");
    assert(!verifyManifest(evil, sign(json, sk), o.key));
    assert(verifyManifest(json, "not base64!", o.key).error() == "malformed signature");
    assert(!verifyManifest(json, sign(json, sk), PublicKey{}));

    // Signed but unsafe artifact paths are still rejected.
    for (const char* bad : {"../evil", "/etc/evil", ".hidden", "a/../b", "a//b", "a\\\\b"}) {
        publish("0.9.0", "", bad);
        assert(!check(base + "/0.9.0/manifest.json", o));
    }

    // Install, then the same version again.
    auto dir = install(*m, o, "0.1.0");
    assert(dir && *dir == o.installRoot / "versions/0.2.0");
    assert(currentVersion(o.installRoot) == "0.2.0" && !previousVersion(o.installRoot));
    assert(slurp(o.installRoot / "current/bbtime") == "game binary 0.2.0");
    assert(slurp(o.installRoot / "current/share/notes.txt") == "data for 0.2.0");
    assert((fs::status(o.installRoot / "current/bbtime").permissions() & fs::perms::owner_exec) != fs::perms::none);
    assert(lastProgress == std::string("game binary 0.2.0data for 0.2.0").size());
    assert(!install(*m, o, "0.1.0"));

    // Tampered artifact: served bytes differ from the signed hash. Nothing changes.
    publish("0.3.0", "", "bbtime", sha256("something else"));
    auto bad = check(base + "/0.3.0/manifest.json", o);
    assert(bad);
    assert(install(*bad, o, "0.1.0").error() == "bbtime: SHA-256 mismatch");
    // Oversized artifact: server sends more than the signed size.
    publish("0.3.0", "", "bbtime", "", 4);
    bad = check(base + "/0.3.0/manifest.json", o);
    assert(bad && install(*bad, o, "0.1.0").error().ends_with("larger than expected"));
    assert(currentVersion(o.installRoot) == "0.2.0" && !fs::exists(o.installRoot / "versions/0.3.0"));
    for (const auto& e : fs::directory_iterator(o.installRoot / "versions"))
        assert(e.path().filename() == "0.2.0"); // no staging left behind

    // Downgrade: refused (also against the running version), allowed with force.
    auto old = check(publish("0.1.5"), o);
    assert(old && install(*old, o, "0.1.0").error().starts_with("refusing downgrade"));
    assert(install(*old, o, "0.2.0").error().starts_with("refusing downgrade"));
    Options forced = o;
    forced.force = true;
    assert(install(*old, forced, "0.2.0"));
    assert(currentVersion(o.installRoot) == "0.1.5" && previousVersion(o.installRoot) == "0.2.0");

    // Upgrade prunes everything but current + previous; rollback swaps them both ways.
    auto up = check(publish("0.3.0"), o);
    assert(up && install(*up, o, "0.1.0"));
    assert(currentVersion(o.installRoot) == "0.3.0" && previousVersion(o.installRoot) == "0.1.5");
    assert(!fs::exists(o.installRoot / "versions/0.2.0"));
    assert(rollback(o.installRoot) == "0.1.5");
    assert(slurp(o.installRoot / "current/bbtime") == "game binary 0.1.5" && previousVersion(o.installRoot) == "0.3.0");
    assert(rollback(o.installRoot) == "0.3.0");
    assert(!rollback(tmp / "nothing"));

    // Release channel default, settings override.
    assert(manifestUrl("") == "https://github.com/sergen213/bugs-bunny-time-linux/releases/latest/download/manifest.json");
    assert(manifestUrl(base + "/x/manifest.json") == base + "/x/manifest.json");
    // Launcher hand-off: only to an executable current/bbtime-launcher that is newer than the running launcher.
    assert(handoffTarget(o.installRoot, "0.1.0").empty()); // 0.3.0 shipped no launcher
    const fs::path launcher = o.installRoot / "current" / kLauncherArtifact;
    put(launcher, "#!/bin/sh\n");
    assert(handoffTarget(o.installRoot, "0.1.0").empty()); // not executable
    fs::permissions(launcher, fs::perms(0755));
    assert(handoffTarget(o.installRoot, "0.1.0") == launcher && handoffTarget(o.installRoot, "0.2.9") == launcher);
    assert(handoffTarget(o.installRoot, "0.3.0").empty() && handoffTarget(o.installRoot, "1.0.0").empty());
    assert(handoffTarget(tmp / "nothing", "0.1.0").empty());
    // Game binary: the installed copy (0.3.0) only for a launcher that is not newer (no stale game from a new AppImage).
    const fs::path game = o.installRoot / "current/bbtime";
    assert(installedArtifact(o.installRoot, "0.2.0", "bbtime") == game && installedArtifact(o.installRoot, "0.3.0", "bbtime") == game);
    assert(installedArtifact(o.installRoot, "0.9.0", "bbtime").empty());
    assert(installedArtifact(o.installRoot, "0.2.0", "bbtime-modkit").empty()); // not installed

    // Version parsing.
    assert(parseVersion("1.2.3") == (Version{1, 2, 3}) && parseVersion("10.0.0") > parseVersion("9.99.99"));
    for (const char* v : {"1.2", "1.2.3.4", "01.2.3", "1.2.x", "-1.2.3", " 1.2.3", "1.2.3 ", ""}) assert(!parseVersion(v));

    // Python release tools -> C++ verify (same Ed25519, raw 64-byte signature).
    const fs::path rel = BBTIME_RELEASE_DIR;
    const std::string py = std::format(
        "cd '{0}' && python3 '{1}/keygen.py' key.pem --pub pub.txt >/dev/null && "
        "python3 '{1}/make_manifest.py' --version 1.0.0 --base-url https://example.invalid/r --exec bbtime "
        "-o manifest.json srv/0.2.0/bbtime 'share/notes.txt=srv/0.2.0/notes.txt' && "
        "python3 '{1}/sign_manifest.py' --key key.pem manifest.json >/dev/null && "
        "python3 '{1}/sign_manifest.py' --verify --pub pub.txt manifest.json >/dev/null",
        tmp.string(), rel.string());
    if (run(py) == 0) {
        const auto key = decodePublicKey(slurp(tmp / "pub.txt"));
        assert(key);
        auto pm = verifyManifest(slurp(tmp / "manifest.json"), slurp(tmp / "manifest.json.sig"), *key);
        assert(pm && pm->version == "1.0.0" && pm->artifacts.size() == 2 && pm->artifacts[0].executable);
        assert(pm->artifacts[0].sha256 == sha256("game binary 0.2.0") && pm->artifacts[1].path == "share/notes.txt");
        assert(!verifyManifest(slurp(tmp / "manifest.json") + "\n", slurp(tmp / "manifest.json.sig"), *key));
    } else {
        std::println("python release scripts unavailable - compatibility check skipped");
    }
    assert(decodePublicKey(slurp(rel / "bbtime-update.pub")) && releasePublicKey() != PublicKey{});

    close(in[1]);
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    fs::remove_all(tmp);
    std::println("updater: all checks passed");
    return 0;
}
