#!/usr/bin/env python3
"""Sign or verify a bbtime release manifest.

sign_manifest.py --key PRIVATE_KEY manifest.json          -> manifest.json.sig (base64 of the raw 64-byte Ed25519
                                                            signature over the exact file bytes; libsodium
                                                            crypto_sign_verify_detached compatible)
sign_manifest.py --verify [--pub bbtime-update.pub] manifest.json
Upload manifest.json and manifest.json.sig unchanged: any byte change (even whitespace) breaks the signature.
"""
import argparse
import base64
import os
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest")
    ap.add_argument("--key", help="PKCS#8 PEM private key from keygen.py")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--pub", default=os.path.join(os.path.dirname(__file__), "bbtime-update.pub"))
    a = ap.parse_args()
    with open(a.manifest, "rb") as f:
        data = f.read()
    if a.verify:
        pub = Ed25519PublicKey.from_public_bytes(base64.b64decode(open(a.pub).read().strip()))
        try:
            pub.verify(base64.b64decode(open(a.manifest + ".sig").read().strip()), data)
        except InvalidSignature:
            sys.exit(f"{a.manifest}: signature INVALID")
        print(f"{a.manifest}: signature OK")
        return 0
    if not a.key:
        sys.exit("--key is required to sign")
    key = serialization.load_pem_private_key(open(a.key, "rb").read(), password=None)
    with open(a.manifest + ".sig", "w") as f:
        f.write(base64.b64encode(key.sign(data)).decode() + "\n")
    print(f"wrote {a.manifest}.sig")
    return 0


if __name__ == "__main__":
    sys.exit(main())
