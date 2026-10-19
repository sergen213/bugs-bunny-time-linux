#!/usr/bin/env python3
"""Create the bbtime release signing key (Ed25519, libsodium-compatible raw signatures).

keygen.py PRIVATE_KEY_FILE [--pub tools/release/bbtime-update.pub]
Writes the private key as PKCS#8 PEM (mode 0600, refuses to overwrite) and the public key as base64 of the raw
32 bytes, which CMake bakes into bbtime-launcher. Keep the private key out of the repository.
"""
import argparse
import base64
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("private_key")
    ap.add_argument("--pub", default=os.path.join(os.path.dirname(__file__), "bbtime-update.pub"))
    a = ap.parse_args()
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    os.makedirs(os.path.dirname(os.path.abspath(a.private_key)), mode=0o700, exist_ok=True)
    fd = os.open(a.private_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)  # never clobber an existing key
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    raw = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    with open(a.pub, "w") as f:
        f.write(base64.b64encode(raw).decode() + "\n")
    print(f"private key: {a.private_key}\npublic key:  {a.pub}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
