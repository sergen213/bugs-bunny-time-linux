# Signed updates

Status: **Implemented**, **Tested** (`launcher.updater` against a local HTTPS server; checked against the real
v0.8.0 release on 2026-10-21). The launcher always uses the release channel
`https://github.com/sergen213/bugs-bunny-time-linux/releases/latest/download/manifest.json` (+ `.sig`); there is no
URL field. `settings.ini` `update_url` overrides it for tests/development only.

## Trust model

* One Ed25519 release key. Public key: `tools/release/bbtime-update.pub` (base64 of the raw 32 bytes), baked into
  `bb_updater` at configure time (`-DBBTIME_UPDATE_PUBKEY_FILE=` to override). The private key was generated with
  `keygen.py` on the development machine at `~/.local/share/bbtime-release/update-ed25519.key` (PKCS#8 PEM,
  mode 0600) and must never enter the repository. Losing it means shipping a launcher with a new public key.
* `manifest.json` is signed byte-for-byte; `manifest.json.sig` = base64 of the raw 64-byte signature. Verification
  is libsodium `crypto_sign_verify_detached` (RFC 8032 Ed25519; Python `cryptography` signs, same algorithm). The
  signature is checked **before** the JSON is parsed.
* Every artifact is pinned by size and SHA-256 inside the signed manifest; a download that exceeds the signed size is
  aborted mid-transfer.
* Transport: libcurl restricted to `https` (also for redirects; GitHub release assets redirect to a CDN), peer and
  host verification always on, system CA store. Non-https URLs (including `file://`) are refused before any
  request. Tests use a throwaway CA via `Options::caFile`; production code has no insecure switch.
* Replay of an old, validly signed manifest is defeated by the version rule: install only if the manifest version
  is strictly greater than both the updater-installed `current` and the running launcher's version, unless the user
  ticks "Allow reinstall/downgrade" (`Options::force`).

## Manifest (schema 1)

```json
{
  "schema": 1,
  "product": "bbtime",
  "version": "0.2.0",
  "notes": "markdown shown in the launcher",
  "artifacts": [
    {"path": "bbtime", "url": "https://…/bbtime", "size": 123, "sha256": "<64 lowercase hex>", "executable": true},
    {"path": "bbtime-launcher", "url": "https://…/bbtime-launcher", "size": 456, "sha256": "…", "executable": true}
  ]
}
```

`version` is strict `X.Y.Z` (no leading zeros). `path` components are `[A-Za-z0-9._-]+`, none starting with `.`
(no `..`, no hidden files, no absolute paths), unique. `product` must be `bbtime`.

## Install layout and swap

```
$XDG_DATA_HOME/bbtime/app/
  versions/0.2.0/…        verified files
  versions/0.3.0/…
  current  -> versions/0.3.0
  previous -> versions/0.2.0
```

1. Download every artifact into `versions/.staging-<ver>-<pid>/` (created exclusively), hashing while writing.
2. Any size/hash/transport error deletes the staging directory; `current` is untouched.
3. Rename staging → `versions/<ver>`; point `current` at it by renaming a fresh symlink over it (atomic); the old
   target becomes `previous`. Versions other than `current`/`previous` are pruned.
4. **Rollback** swaps `current` and `previous` (both must exist).

The updater writes only below `app/`; game data, copied `Datas/`, saves and original installs are never touched.
The launcher starts `app/current/bbtime` (and `bbtime-modkit`) only when `current` is at least as new as the running
launcher (`installedArtifact`); otherwise it runs the copy bundled with it, so a newer AppImage never starts an older
installed game.

## Launcher restart and hand-off

Releases from 0.9.0 on also ship the AppImage itself as the artifact `bbtime-launcher` (installed as
`app/current/bbtime-launcher`, executable). After a successful install whose `current` is newer than the running
launcher, the launcher saves its settings, quits and `execv`s `app/current/bbtime-launcher` with its original
arguments. On every normal start, a launcher whose version is lower than `current` hands off the same way
(`bbtime::update::handoffTarget`), so the desktop entry / old AppImage the user starts always ends up in the newest
installed launcher. Loop guard: the handing-off launcher sets `BBTIME_LAUNCHER_HANDOFF=1`; a launcher started with
it never hands off again (and clears it). A rollback or forced downgrade makes `current` older than the running
launcher, so no hand-off happens. Releases without the artifact (0.8.0) only update the game binary.

## Releasing

```sh
# bump `project(bbtime VERSION X.Y.Z …)` in CMakeLists.txt and the CHANGELOG heading, commit, then:
tools/release/build_appimage.sh X.Y.Z   # → out/release-X.Y.Z/
git tag vX.Y.Z && git push origin main vX.Y.Z
gh release create vX.Y.Z -R sergen213/bugs-bunny-time-linux --notes-file NOTES.md out/release-X.Y.Z/*
```

The script builds in the `tools/release/appimage/Dockerfile` container (Ubuntu 24.04, g++-14, Qt 6.4), packs
`bbtime-launcher` + `bbtime` into `bbtime-X.Y.Z-x86_64.AppImage` with linuxdeploy, and signs `manifest.json` with the
private key. The manifest lists `bbtime` (the game, preferred by every launcher as `app/current/bbtime`) and the
AppImage as `bbtime-launcher` (`make_manifest.py … "bbtime-launcher=$OUT/bbtime-X.Y.Z-x86_64.AppImage"`, its URL is
the AppImage asset of the release). The `.sig` is fetched from the manifest URL plus `.sig`. Tools need `python3` +
`cryptography` (installed).

## Tests (`launcher.updater`)

Local HTTPS server (`apps/launcher/tests/https_server.py`, self-signed cert for 127.0.0.1) and a fresh libsodium key:
valid manifest; `http://` and `file://` refused; untrusted certificate refused; wrong key, tampered bytes, malformed
signature and missing key rejected; signed manifests with `../`, absolute, hidden, `a/../b`, empty-component and
backslash paths rejected; install + exec bit + progress; same version refused; tampered artifact (hash mismatch)
and oversized artifact leave `current` and `versions/` unchanged with no staging left; downgrade refused against
both installed and running version; forced downgrade; upgrade prunes; rollback both ways; the default release URL
and the `update_url` override; launcher hand-off only to an executable `current/bbtime-launcher` newer than the
running version; version parser; and the
Python release scripts (keygen → make → sign → verify) cross-checked with the C++ verifier. Exit 77 (skip) if
`python3`/`cryptography` is missing.
