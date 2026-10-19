#!/usr/bin/env python3
"""Write an unsigned bbtime release manifest (schema 1, docs/UPDATES.md).

make_manifest.py --version X.Y.Z --base-url https://github.com/OWNER/bbtime/releases/download/vX.Y.Z \
    [--notes-file NOTES.md] [--exec bbtime --exec bbtime-launcher] -o manifest.json FILE...
Each FILE becomes an artifact at its basename (or `install/path=FILE` to choose the install path).
"""
import argparse
import hashlib
import json
import os
import re
import sys

PATH_RE = re.compile(r"^(?!\.)[A-Za-z0-9._-]+(/(?!\.)[A-Za-z0-9._-]+)*$")  # same rule as updater.cpp validPath


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--notes-file")
    ap.add_argument("--exec", action="append", default=[], help="install path to mark executable")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", a.version):
        sys.exit("version must be X.Y.Z")
    if not a.base_url.startswith("https://"):
        sys.exit("base URL must be https://")
    artifacts = []
    for spec in a.files:
        path, _, src = spec.partition("=") if "=" in spec else (os.path.basename(spec), "", spec)
        if not PATH_RE.fullmatch(path):
            sys.exit(f"bad install path: {path}")
        with open(src, "rb") as f:
            data = f.read()
        artifacts.append({
            "path": path,
            "url": f"{a.base_url.rstrip('/')}/{os.path.basename(src)}",
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "executable": path in a.exec,
        })
    notes = open(a.notes_file, encoding="utf-8").read() if a.notes_file else ""
    manifest = {"schema": 1, "product": "bbtime", "version": a.version, "notes": notes, "artifacts": artifacts}
    with open(a.output, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
