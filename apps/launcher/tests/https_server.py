#!/usr/bin/env python3
"""Test-only HTTPS file server: https_server.py ROOT CERT_OUT
Generates a throwaway self-signed certificate for 127.0.0.1 (written to CERT_OUT for curl's CAINFO), serves ROOT on
an ephemeral port, prints the port on stdout, and exits when stdin closes. Exit 77 if `cryptography` is missing.
"""
import datetime
import functools
import http.server
import ipaddress
import os
import ssl
import sys
import tempfile
import threading

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
except ImportError:
    sys.exit(77)


def main() -> None:
    root, cert_out = sys.argv[1], sys.argv[2]
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(hours=1))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), False)
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), True)
            .sign(key, hashes.SHA256()))
    pem = cert.public_bytes(serialization.Encoding.PEM)
    with open(cert_out, "wb") as f:
        f.write(pem)
    with tempfile.NamedTemporaryFile("wb", delete=False) as kf:
        kf.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                   serialization.NoEncryption()))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_out, kf.name)
    os.unlink(kf.name)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=root))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    httpd.handle_error = lambda *_: None  # clients abort on purpose (oversize, untrusted cert)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    print(httpd.server_address[1], flush=True)
    sys.stdin.read()  # parent closes the pipe (or dies) -> shut down
    httpd.shutdown()


if __name__ == "__main__":
    main()
