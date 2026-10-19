# Signed updates

Status: **Implemented**, **Tested** (`launcher.updater` against a local HTTPS server). No public release repository
exists yet, so the launcher's `update_url` is empty by default; nothing has been published.

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
The launcher prefers `app/current/bbtime` when starting the game.

## Releasing

```sh
python3 tools/release/make_manifest.py --version 0.2.0 \
    --base-url https://github.com/OWNER/bbtime/releases/download/v0.2.0 \
    --notes-file notes.md --exec bbtime --exec bbtime-launcher -o manifest.json build/apps/bbtime/bbtime \
    build/apps/launcher/bbtime-launcher
python3 tools/release/sign_manifest.py --key ~/.local/share/bbtime-release/update-ed25519.key manifest.json
python3 tools/release/sign_manifest.py --verify manifest.json
# upload the artifacts, manifest.json and manifest.json.sig to the release unchanged
```

Users set `update_url = https://github.com/OWNER/bbtime/releases/latest/download/manifest.json`; the `.sig` is
fetched from the same URL plus `.sig`. Tools need `python3` + `cryptography` (installed).

## Tests (`launcher.updater`)

Local HTTPS server (`apps/launcher/tests/https_server.py`, self-signed cert for 127.0.0.1) and a fresh libsodium key:
valid manifest; `http://` and `file://` refused; untrusted certificate refused; wrong key, tampered bytes, malformed
signature and missing key rejected; signed manifests with `../`, absolute, hidden, `a/../b`, empty-component and
backslash paths rejected; install + exec bit + progress; same version refused; tampered artifact (hash mismatch)
and oversized artifact leave `current` and `versions/` unchanged with no staging left; downgrade refused against
both installed and running version; forced downgrade; upgrade prunes; rollback both ways; version parser; and the
Python release scripts (keygen → make → sign → verify) cross-checked with the C++ verifier. Exit 77 (skip) if
`python3`/`cryptography` is missing.
