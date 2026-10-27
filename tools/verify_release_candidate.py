#!/usr/bin/env python3
"""Verify that release promotion uses the exact reviewed, signed candidate bytes."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ota_publish', ROOT/'tools/ota/publish.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)

def verify(directory, version, source_digest):
    metadata = json.loads((directory/'REVIEW.json').read_text())
    if metadata.get('schema') != 1 or metadata.get('version') != version:
        raise ValueError('Candidate version/schema differs')
    if metadata.get('source_digest') != source_digest:
        raise ValueError('Candidate was built from a different source tree')
    if not re.fullmatch('[a-f0-9]{40}', metadata.get('source_commit', '')):
        raise ValueError('Missing candidate source commit')
    data = (directory/'firmware.bin').read_bytes()
    if hashlib.sha256(data).hexdigest() != metadata.get('firmware_sha256'):
        raise ValueError('Candidate firmware hash differs')
    payload = ota.verify_manifest(json.loads((directory/'latest.json').read_text()))
    expected = ota.manifest_payload(data, 'public', version, 'n16r8-rev4', 'n16r8-ota-v1')
    if payload != expected:
        raise ValueError('Signed manifest does not describe the selected public firmware')
    checksums = {}
    for line in (directory/'SHA256SUMS.txt').read_text().splitlines():
        match = re.fullmatch(r'([a-f0-9]{64})  ([A-Za-z0-9_.-]+)', line)
        if not match or match[2] in checksums:
            raise ValueError('Invalid/duplicate checksum entry')
        checksums[match[2]] = match[1]
    required = {'firmware.bin', 'latest.json', 'REVIEW.json', 'REVIEW.txt',
                'LICENSE', 'THIRD_PARTY_NOTICES.md', 'source-review.tar.gz', 'NOTICES-REVIEW.txt'}
    if not required.issubset(checksums):
        raise ValueError('Candidate is missing release materials')
    for name, digest in checksums.items():
        if hashlib.sha256((directory/name).read_bytes()).hexdigest() != digest:
            raise ValueError('Candidate asset checksum differs: '+name)
    return metadata

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    digest = subprocess.check_output(['python3', str(ROOT/'tools/check_release_approval.py'), '--digest'], text=True).strip()
    metadata = verify(args.directory, args.version, digest)
    print('Verified exact candidate bytes from source commit', metadata['source_commit'])
