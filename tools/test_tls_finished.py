#!/usr/bin/env python3
"""Build and fault-test the real TLS unit using the checksum-pinned SDK source."""
import hashlib
from pathlib import Path
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9'
SHA256 = '7c47e87902c9153706fc469a5f7b609bd67c02e00e4ac3c977137bc74b5507d9'
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    url = f'https://codeload.github.com/espressif/mbedtls/tar.gz/{COMMIT}'
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(32 * 1024 * 1024 + 1)
    assert hashlib.sha256(data).hexdigest() == SHA256, 'SDK source checksum mismatch'
    archive = temp/'source.tar.gz'
    archive.write_bytes(data)
    with tarfile.open(archive) as source:
        source.extractall(temp, filter='data')
    source = temp/f'mbedtls-{COMMIT}'
    build = temp/'build'
    subprocess.run(['cmake', '-S', str(source), '-B', str(build),
                    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_TESTING=OFF',
                    '-DENABLE_PROGRAMS=OFF'], check=True)
    subprocess.run(['cmake', '--build', str(build), '-j', '4'], check=True)
    executable = temp/'test-finished'
    subprocess.run(['cc', '-std=c99', '-fsanitize=address,undefined',
                    '-I', str(source/'include'), str(ROOT/'tests/tls_finished.c'),
                    *[str(build/'library'/f'lib{name}.a') for name in
                      ('mbedtls', 'mbedx509', 'mbedcrypto')], '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: TLS Finished hash/PRF errors propagate; read/write abort without advancing')
