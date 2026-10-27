import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('candidate', ROOT/'tools/verify_release_candidate.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)

class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.assets = self.root/'assets'; self.assets.mkdir()
        key = self.root/'key.pem'; public = self.root/'public.pem'
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(key)],check=True,capture_output=True)
        public.write_bytes(subprocess.check_output(['openssl','pkey','-in',str(key),'-pubout']))
        original = candidate.ota.verify_manifest
        verifier = patch.object(candidate.ota,'verify_manifest',lambda e: original(e, public_key=public))
        verifier.start(); self.addCleanup(verifier.stop)
        data = bytes([0xe9])+bytes(31)+bytes.fromhex('3254cdab')+bytes(256)+b'v1.0.0\0'
        (self.assets/'firmware.bin').write_bytes(data)
        self.digest = 'a'*64
        metadata = dict(schema=1,version='v1.0.0',source_commit='b'*40,
                        source_digest=self.digest,firmware_sha256=hashlib.sha256(data).hexdigest())
        (self.assets/'REVIEW.json').write_text(json.dumps(metadata))
        payload = json.dumps(candidate.ota.manifest_payload(data,'public','v1.0.0','n16r8-rev4','n16r8-ota-v1'),sort_keys=True,separators=(',',':'))
        signature = subprocess.run(['openssl','dgst','-sha256','-sign',str(key)],input=payload.encode(),check=True,capture_output=True).stdout
        (self.assets/'latest.json').write_text(json.dumps(dict(payload=payload,signature=base64.b64encode(signature).decode())))
        for name in ('REVIEW.txt','LICENSE','THIRD_PARTY_NOTICES.md','source-review.tar.gz','NOTICES-REVIEW.txt'):
            (self.assets/name).write_bytes(b'test release material')
        self.checksums()

    def checksums(self):
        paths=sorted(p for p in self.assets.iterdir() if p.name!='SHA256SUMS.txt')
        (self.assets/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n' for p in paths))

    def test_exact_candidate(self):
        candidate.verify(self.assets,'v1.0.0',self.digest)

    def test_source_mismatch(self):
        with self.assertRaises(ValueError): candidate.verify(self.assets,'v1.0.0','c'*64)

    def test_tampered_binary_even_with_updated_checksums(self):
        with (self.assets/'firmware.bin').open('ab') as f: f.write(b'changed')
        self.checksums()
        with self.assertRaises(ValueError): candidate.verify(self.assets,'v1.0.0',self.digest)

    def test_tampered_signed_payload(self):
        p=self.assets/'latest.json';p.write_text(p.read_text().replace('public','private'))
        self.checksums()
        with self.assertRaises(subprocess.CalledProcessError): candidate.verify(self.assets,'v1.0.0',self.digest)

    def test_missing_source(self):
        (self.assets/'source-review.tar.gz').unlink();self.checksums()
        with self.assertRaises(ValueError): candidate.verify(self.assets,'v1.0.0',self.digest)

    def test_altered_notices(self):
        (self.assets/'NOTICES-REVIEW.txt').write_text('changed')
        with self.assertRaises(ValueError): candidate.verify(self.assets,'v1.0.0',self.digest)

    def test_path_traversal(self):
        with (self.assets/'SHA256SUMS.txt').open('a') as f: f.write('d'*64+'  ../outside\n')
        with self.assertRaises(ValueError): candidate.verify(self.assets,'v1.0.0',self.digest)
