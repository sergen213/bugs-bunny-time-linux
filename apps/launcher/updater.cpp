#include "updater.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <format>
#include <mutex>
#include <set>
#include <unistd.h>

namespace fs = std::filesystem;

namespace bbtime::update {
namespace {

void initLibs() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) std::abort();
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

bool validPath(std::string_view p) {
    if (p.empty() || p.size() > 255) return false;
    for (size_t pos = 0; pos <= p.size();) {
        size_t end = p.find('/', pos);
        if (end == p.npos) end = p.size();
        const std::string_view c = p.substr(pos, end - pos);
        if (c.empty() || c[0] == '.') return false; // also rejects "." and ".."
        for (char ch : c)
            if (!(std::isalnum(uint8_t(ch)) || ch == '.' || ch == '_' || ch == '-')) return false;
        pos = end + 1;
    }
    return true;
}

bool isHex64(std::string_view s) {
    return s.size() == 64 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<std::string> linkVersion(const fs::path& link) {
    std::error_code ec;
    const fs::path t = fs::read_symlink(link, ec);
    if (ec || !parseVersion(t.filename().string())) return std::nullopt;
    return t.filename().string();
}

// Atomically points `link` at `target` (relative) via rename(2) of a fresh symlink.
std::string swapLink(const fs::path& link, const fs::path& target) {
    const fs::path tmp = link.string() + std::format(".tmp{}", getpid());
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::create_symlink(target, tmp, ec);
    if (!ec) fs::rename(tmp, link, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return std::format("cannot update {}", link.string());
    }
    return {};
}

struct Sink {
    std::string* mem = nullptr;
    FILE* file = nullptr;
    crypto_hash_sha256_state* sha = nullptr;
    uint64_t limit = 0, got = 0, base = 0, total = 0;
    bool tooBig = false;
    const Options* o = nullptr;
};

size_t onData(char* p, size_t, size_t n, void* user) {
    auto& s = *static_cast<Sink*>(user);
    if (s.got + n > s.limit) return s.tooBig = true, 0; // aborts the transfer (CURLE_WRITE_ERROR)
    s.got += n;
    if (s.mem) s.mem->append(p, n);
    if (s.file && std::fwrite(p, 1, n, s.file) != n) return 0;
    if (s.sha) crypto_hash_sha256_update(s.sha, reinterpret_cast<unsigned char*>(p), n);
    if (s.o && s.o->progress) s.o->progress(s.base + s.got, s.total);
    return n;
}

std::string get(const std::string& url, const Options& o, Sink& sink) {
    if (!url.starts_with("https://")) return std::format("refusing non-https URL: {}", url);
    initLibs();
    CURL* c = curl_easy_init();
    if (!c) return "curl_easy_init failed";
    char err[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); // GitHub release assets redirect to a CDN
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!o.caFile.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, o.caFile.c_str());
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "bbtime-launcher");
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK) return {};
    if (sink.tooBig) return std::format("{}: larger than expected", url);
    return std::format("{}: {}", url, *err ? err : curl_easy_strerror(rc));
}

} // namespace

std::optional<Version> parseVersion(std::string_view s) {
    Version v;
    int* parts[] = {&v.major, &v.minor, &v.patch};
    const char *p = s.data(), *end = s.data() + s.size();
    for (int i = 0; i < 3; ++i) {
        if (p == end || !std::isdigit(uint8_t(*p)) || (*p == '0' && p + 1 != end && std::isdigit(uint8_t(p[1]))))
            return std::nullopt; // no sign, no leading zeros
        const auto r = std::from_chars(p, end, *parts[i]);
        if (r.ec != std::errc{}) return std::nullopt;
        p = r.ptr;
        if (i < 2) {
            if (p == end || *p != '.') return std::nullopt;
            ++p;
        }
    }
    return p == end ? std::optional(v) : std::nullopt;
}

std::optional<PublicKey> decodePublicKey(std::string_view b64) {
    initLibs();
    PublicKey k{};
    size_t len = 0;
    if (sodium_base642bin(k.data(), k.size(), b64.data(), b64.size(), " \r\n", &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 || len != k.size())
        return std::nullopt;
    return k;
}

PublicKey releasePublicKey() { return decodePublicKey(BBTIME_UPDATE_PUBKEY).value_or(PublicKey{}); }

std::expected<Manifest, std::string> verifyManifest(std::string_view json, std::string_view sigB64, const PublicKey& key) {
    initLibs();
    if (key == PublicKey{}) return std::unexpected("no update public key configured");
    unsigned char sig[crypto_sign_BYTES];
    size_t len = 0;
    if (sodium_base642bin(sig, sizeof sig, sigB64.data(), sigB64.size(), " \r\n", &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 || len != sizeof sig)
        return std::unexpected("malformed signature");
    if (crypto_sign_verify_detached(sig, reinterpret_cast<const unsigned char*>(json.data()), json.size(), key.data()) != 0)
        return std::unexpected("manifest signature does not verify");

    // Signed from here on: remaining checks guard against release-tool mistakes, not attackers.
    Manifest m;
    try {
        const auto j = nlohmann::json::parse(json);
        if (j.at("schema").get<int>() != 1) return std::unexpected("unsupported manifest schema");
        if (j.at("product").get<std::string>() != "bbtime") return std::unexpected("manifest is for another product");
        m.version = j.at("version").get<std::string>();
        if (!parseVersion(m.version)) return std::unexpected("bad manifest version");
        m.notes = j.value("notes", std::string{});
        std::set<std::string> seen;
        for (const auto& a : j.at("artifacts")) {
            Artifact x{a.at("path").get<std::string>(), a.at("url").get<std::string>(), a.at("size").get<uint64_t>(),
                       a.at("sha256").get<std::string>(), a.value("executable", false)};
            if (!validPath(x.path) || !seen.insert(x.path).second) return std::unexpected("bad artifact path: " + x.path);
            if (!x.url.starts_with("https://")) return std::unexpected("non-https artifact URL: " + x.url);
            if (!isHex64(x.sha256)) return std::unexpected("bad sha256 for " + x.path);
            m.artifacts.push_back(std::move(x));
        }
        if (m.artifacts.empty()) return std::unexpected("manifest has no artifacts");
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(std::format("bad manifest JSON: {}", e.what()));
    }
    return m;
}

std::expected<std::string, std::string> fetch(const std::string& url, const Options& o, size_t maxBytes) {
    std::string body;
    Sink s{.mem = &body, .limit = maxBytes};
    if (std::string e = get(url, o, s); !e.empty()) return std::unexpected(e);
    return body;
}

std::expected<Manifest, std::string> check(const std::string& manifestUrl, const Options& o) {
    Options quiet = o;
    quiet.progress = nullptr;
    auto json = fetch(manifestUrl, quiet, 1 << 20);
    if (!json) return std::unexpected(json.error());
    auto sig = fetch(manifestUrl + ".sig", quiet, 1024);
    if (!sig) return std::unexpected(sig.error());
    return verifyManifest(*json, *sig, o.key);
}

std::optional<std::string> currentVersion(const fs::path& root) { return linkVersion(root / "current"); }
std::optional<std::string> previousVersion(const fs::path& root) { return linkVersion(root / "previous"); }

fs::path handoffTarget(const fs::path& root, std::string_view running) {
    const auto cur = currentVersion(root);
    const auto have = parseVersion(cur.value_or("")), run = parseVersion(running);
    const fs::path exe = root / "current" / kLauncherArtifact;
    if (!have || (run && *have <= *run) || access(exe.c_str(), X_OK) != 0) return {};
    return exe;
}

std::expected<fs::path, std::string> install(const Manifest& m, const Options& o, std::string_view running) {
    const auto target = parseVersion(m.version);
    if (!target) return std::unexpected("bad manifest version");
    const auto cur = currentVersion(o.installRoot);
    const Version base = std::max(parseVersion(cur.value_or("0.0.0")).value_or(Version{}),
                                  parseVersion(running).value_or(Version{}));
    if (cur == m.version) return std::unexpected(std::format("{} is already installed", m.version));
    if (!o.force && *target <= base)
        return std::unexpected(std::format("refusing {} {}: installed is {}.{}.{} (use force)",
                                           *target == base ? "reinstall of" : "downgrade to", m.version, base.major,
                                           base.minor, base.patch));

    const fs::path versions = o.installRoot / "versions";
    const fs::path staging = versions / std::format(".staging-{}-{}", m.version, getpid());
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", staging.string(), ec.message()));
    auto fail = [&](std::string e) { fs::remove_all(staging, ec); return std::unexpected(std::move(e)); };

    uint64_t total = 0, done = 0;
    for (const Artifact& a : m.artifacts) total += a.size;
    for (const Artifact& a : m.artifacts) {
        const fs::path dst = staging / a.path;
        fs::create_directories(dst.parent_path(), ec);
        FILE* f = std::fopen(dst.c_str(), "wbx");
        if (!f) return fail(std::format("cannot create {}", dst.string()));
        crypto_hash_sha256_state sha;
        crypto_hash_sha256_init(&sha);
        Sink s{.file = f, .sha = &sha, .limit = a.size, .base = done, .total = total, .o = &o};
        std::string e = get(a.url, o, s);
        if (std::fclose(f) != 0 && e.empty()) e = std::format("cannot write {}", dst.string());
        if (!e.empty()) return fail(e);
        unsigned char digest[crypto_hash_sha256_BYTES];
        crypto_hash_sha256_final(&sha, digest);
        char hex[2 * sizeof digest + 1];
        sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
        if (s.got != a.size) return fail(std::format("{}: size {} != {}", a.path, s.got, a.size));
        if (hex != a.sha256) return fail(std::format("{}: SHA-256 mismatch", a.path));
        fs::permissions(dst, a.executable ? fs::perms(0755) : fs::perms(0644), ec);
        done += a.size;
    }

    const fs::path dest = versions / m.version;
    fs::remove_all(dest, ec); // a stale copy (never `current`, checked above): replaced by the verified download
    fs::rename(staging, dest, ec);
    if (ec) return fail(std::format("cannot move {} into place: {}", dest.string(), ec.message()));
    if (std::string e = swapLink(o.installRoot / "current", fs::path("versions") / m.version); !e.empty())
        return std::unexpected(e);
    if (cur) swapLink(o.installRoot / "previous", fs::path("versions") / *cur);

    // ponytail: keeps only current + previous; keep N if users ask for deeper rollback.
    const auto prev = previousVersion(o.installRoot);
    std::vector<fs::path> stale;
    for (const auto& e : fs::directory_iterator(versions, ec)) {
        const std::string n = e.path().filename().string();
        if (n != m.version && n != prev && !n.starts_with(".")) stale.push_back(e.path());
    }
    for (const fs::path& p : stale) fs::remove_all(p, ec);
    return dest;
}

std::expected<std::string, std::string> rollback(const fs::path& root) {
    const auto cur = currentVersion(root), prev = previousVersion(root);
    if (!cur || !prev) return std::unexpected("no previous version to roll back to");
    if (!fs::is_directory(root / "versions" / *prev)) return std::unexpected(std::format("versions/{} is missing", *prev));
    if (std::string e = swapLink(root / "current", fs::path("versions") / *prev); !e.empty()) return std::unexpected(e);
    swapLink(root / "previous", fs::path("versions") / *cur);
    return *prev;
}

} // namespace bbtime::update
