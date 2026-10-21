"""Build the ESP-IDF x509 certificate bundle from a PEM trust store.

The bundle is what WiFiClientSecure::setCACertBundle / WebSockets
beginSslWithBundle verify against: a big-endian certificate count, then per
root (sorted by DER subject for binary search) the subject and public-key
lengths followed by the subject DER and SubjectPublicKeyInfo DER. A handshake
then checks one signature against one looked-up key instead of parsing the
whole PEM store on every connection.

  python3 tools/gen_crt_bundle.py certs/mozilla.pem certs/mozilla.bundle
  python3 tools/gen_crt_bundle.py --check certs/mozilla.pem certs/mozilla.bundle
"""
import argparse
import struct
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization


def build(pem: bytes) -> bytes:
    certs = x509.load_pem_x509_certificates(pem)
    entries = {}
    for cert in certs:
        subject = cert.subject.public_bytes()
        key = cert.public_key().public_bytes(serialization.Encoding.DER,
                                             serialization.PublicFormat.SubjectPublicKeyInfo)
        if subject in entries and entries[subject] != key:
            sys.exit(f"ambiguous subject with two keys: {cert.subject.rfc4514_string()}")
        entries[subject] = key
    out = struct.pack(">H", len(entries))
    for subject in sorted(entries):
        key = entries[subject]
        out += struct.pack(">HH", len(subject), len(key)) + subject + key
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the bundle is stale")
    parser.add_argument("pem", type=Path)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    bundle = build(args.pem.read_bytes())
    if args.check:
        if not args.bundle.exists() or args.bundle.read_bytes() != bundle:
            print(f"STALE {args.bundle}: regenerate with tools/gen_crt_bundle.py {args.pem} {args.bundle}")
            return 1
        print(f"PASS {args.bundle} matches {args.pem} ({struct.unpack('>H', bundle[:2])[0]} roots, {len(bundle)} B)")
        return 0
    args.bundle.write_bytes(bundle)
    print(f"wrote {args.bundle}: {struct.unpack('>H', bundle[:2])[0]} roots, {len(bundle)} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
